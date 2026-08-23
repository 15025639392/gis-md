#pragma once

#include "FeatureStore.h"
#include "FeatureTileMesh.h"
#include "MvtDecoder.h"
#include "MvtFeatureConverter.h"
#include "MvtTileFetchCache.h"
#include "StyleFilter.h"
#include "VectorTileTree.h"
#include "../core/async/WorkLedger.h"
#include "../tiling/TileKey.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <tuple>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace earth_engine {

class ThreadPool;

/// 只读矢量底图源(P4c 泛化,E3):VectorTileTree 选择 + 拉取/解码调度 +
/// worker 全链镶嵌,渲染由外挂既有 FeatureRenderLayer 完成
/// ("两种源一条下游",设计 §4——fill/line/point/label/stencil/样式
/// 表达式/snap 全部复用,矢量源侧零渲染代码)。
///
/// **E1:瓦片即桶,worker 全链镶嵌。** 拉取 → 解码 → **镶嵌**全在 worker
/// 完成,渲染线程只做「上传 + 整瓦原子替换」。相比原先灌 FeatureStore 的
/// 做法,这条通路没有 store、没有脏桶差分、没有激活预算 —— 那三样都是为了
/// 让「整视口要素一次性写进空间分桶 store」不把渲染线程钉死而生的补丁
/// (P4 真机实测 debug 下 16s/帧),根因是拿编辑用的差分结构承载只读底图。
///
/// **E3 泛化(E 方案通路):载荷类型模板化。** MVT 与高德 Nebula 共用同一
/// 套调度(树选择/请求去重/worker 镶嵌/渲染线程 commit/drop 差分换手),
/// 差异只在:
///   - Payload:解码产物的容器(MvtTile,或 amap 的 Feature 列表);
///   - DecodeTraits:字节流 → Payload 的解码器 + 近似字节统计;
///   - ToFeaturesFn:Payload → 引擎 Feature 列表(样式层过滤可在这里做)。
/// 现有 MVT 消费方用别名 `MvtVectorSource` 零改动接入。
///
/// 渲染集语义不变:瓦片进入 renderTiles 时 commit 网格,退出时 drop ——
/// 多 zoom 的 LRU 缓存瓦片若常驻会永久叠画(z8 路网压 z12 路网)。解码后
/// 的载荷缓存归树的 LRU,重新激活只重镶嵌、零重拉取/重解码。祖先回退
/// 期的粗细并存是加载暂态,与 maplibre 行为一致。
///
/// 线程契约:update() 必须在渲染线程调用(commit/drop 要 GL 上下文);
/// fetch 回调/解码/镶嵌任意线程,结果经收件箱在 update() 内消化。析构后
/// 迟到的回调安全(shared_ptr 收件箱)。
template <typename Payload, typename DecodeTraits, typename ToFeaturesFn>
class VectorTileSourceT {
public:
    /// 获取层单一化(符号刀A.5):瓦片获取/解码/在途去重/LRU 全部经
    /// MvtTileFetchCacheT —— 与 drape(面)/路网场(线)共享同一实例时,
    /// 同一块数据瓦网络恰一次、解码恰一次、内存恰一份。本类不再自带
    /// fetch+decode 栈(旧 FetchFn 路径已删,测试用假 FetchFn 建 cache)。

    struct Options {
        typename VectorTileTreeT<Payload>::Options tree;
        /// 只导入这些源图层;空 = 全部。与 layerRules 的关系:includeLayers
        /// 是粗筛(层名白名单),layerRules 是细则(zoom 区间 + 逐要素过滤)。
        /// 两者都设时先过白名单再过细则。
        std::vector<std::string> includeLayers;
        /// E2 样式层 runtime filter:把「哪些层在哪些 zoom 画、层内画哪些
        /// 要素」从数据侧搬回样式侧。空 = 不过滤(该层全收)。
        ///
        /// 背景:P4 是用 tippecanoe 的 -j 把道路分级烘死在瓦片里,换一次
        /// 分级策略就得重切整套瓦片,同一份数据也没法给不同样式复用。
        /// 对齐 maplibre:数据只管密度,样式管取舍。
        ///
        /// 规则在 **worker 线程**求值(StyleFilter 是不可变纯函数);改规则
        /// 需调用 setLayerRules,它会把已 commit 的瓦片全部标脏重镶。
        std::vector<SourceLayerRule> layerRules;
        /// 单帧最多 commit 几块瓦片(0 = 不限)。这不是「镶嵌预算」——
        /// 镶嵌已在 worker,渲染线程只剩上传。留这个闸是因为**上传本身**
        /// 有成本(buffer 创建 + 传输),整视口瓦片同帧上传仍会顶出尖刺。
        /// 与旧的 maxActivationFeaturesPerUpdate 不同:那个拦的是镶嵌,
        /// 是结构性补丁;这个拦的是上传,是正常的帧预算。
        size_t maxTileCommitsPerUpdate = 4;
    };

    /// worker 侧镶嵌钩子:把一块瓦片的要素镶成网格。由调用方绑定
    /// (典型:FeatureRenderLayer::tessellateTileMesh + 一份样式快照)。
    /// **必须线程安全且不碰渲染线程状态** —— 它在解码线程上跑。
    ///
    /// 带 key:贴地体的高度范围要按**这块瓦片自己的矩形**取(全屏一个 union
    /// 会让平原上的路背着山地的相对高差,体高直接换算成 fill)。key 是本函数
    /// 唯一能拿到"我是哪一块"的途径 —— Feature 列表里没有瓦片身份。
    using TessellateFn =
        std::function<FeatureTileMesh(const TileKey&, std::vector<Feature>&&)>;
    /// 渲染线程侧网格落地钩子(典型:FeatureRenderLayer::commitTileMesh)。
    using CommitFn = std::function<void(const TileKey&, FeatureTileMesh&&)>;
    /// 渲染线程侧移除钩子(典型:FeatureRenderLayer::dropTileMesh)。
    using DropFn = std::function<void(const TileKey&)>;

    struct Sinks {
        TessellateFn tessellate;
        CommitFn commit;
        DropFn drop;
    };

    /// tileCache:获取层(fetch+解码+在途去重+LRU)。与其他消费方共享同一
    /// 实例即获得跨消费方去重;解码在 cache 的 fetch 回调线程完成。
    /// decodePool 为空则镶嵌在 cache 回调线程就地跑(仍不占渲染线程)。
    /// decodePool 用 shared_ptr:回调可能晚于宿主拆除 —— 回调里对 pool 只持
    /// weak,锁不上就丢弃工作。裸指针版真机崩过(tombstone_22:回调 enqueue
    /// 已析构线程池 = destroyed mutex abort)。
    VectorTileSourceT(Options options, Sinks sinks,
                      std::shared_ptr<MvtTileFetchCacheT<Payload, DecodeTraits>>
                          tileCache,
                      std::shared_ptr<ThreadPool> decodePool = nullptr,
                      ToFeaturesFn toFeatures = ToFeaturesFn{})
        : options_(std::move(options)),
          sinks_(std::move(sinks)),
          tileCache_(std::move(tileCache)),
          decodePool_(std::move(decodePool)),
          toFeatures_(std::move(toFeatures)),
          tree_(options_.tree),
          inbox_(std::make_shared<Inbox>()) {}

    /// 渲染线程每帧调用:驱动树、发缺瓦片请求、消化 worker 产物、
    /// 按渲染集差分 commit/drop 瓦片网格。
    void update(const Rectangle& viewRect, double cameraHeightMeters);

    /// P6 分段:上一次 update 的耗时构成。慢帧归因用 —— "时间不在引擎"
    /// 之后还要能再往下切一刀,否则只能停在猜测。
    struct UpdateStats {
        double ingestMs = 0.0;    ///< 收件箱搬运(解码瓦/网格入账)
        double treeMs = 0.0;      ///< 选择(视口枚举 + 回退)
        double dispatchMs = 0.0;  ///< 请求发起 + 镶嵌派单
        double commitMs = 0.0;    ///< **渲染线程**上传:commit/drop 差分
        int commits = 0;
        int drops = 0;
        int tessellateDispatched = 0;
    };
    const UpdateStats& lastUpdateStats() const { return lastStats_; }

    /// 相机地平线圆的经纬包围矩形(视口 viewRect 的标准来源;数学与
    /// FeatureRenderLayer::visibleBucketKeys 同源:两侧地平线角相加 +
    /// 球冠经度包围界)。跨反经线时返回 west > east 的跨界矩形,
    /// update/VectorTileTree 原生消化。
    static Rectangle horizonViewRectangle(const Cartographic& cameraCarto,
                                          double ellipsoidMinRadiusMeters);

    /// 改样式层规则(渲染线程)。已 commit 的瓦片全部丢弃重镶 —— 网格是
    /// 样式相关的派生物,规则变了就不再有效;解码结果留在树的 LRU,故重镶
    /// 零重拉取。
    void setLayerRules(std::vector<SourceLayerRule> rules);

    VectorTileTreeT<Payload>& tree() { return tree_; }
    /// 已 commit 的瓦片数(诊断)。
    size_t activeTileCount() const { return activeTiles_.size(); }

    /// 是否还有瓦片在 worker 上镶嵌(产物尚未回到渲染线程)。帧级按需渲染
    /// 用:停帧会让 update() 不再被调用 → 收件箱永远没人排空 → 该瓦片
    /// 永远 commit 不上去,底图停在半成品且零报错。
    bool hasTessellationInFlight() const { return !tessellating_.empty(); }
    /// 已镶好、等待 commit 的瓦片数(诊断:持续 >0 说明上传闸偏紧)。
    size_t pendingCommitCount() const { return readyMeshes_.size(); }

private:
    struct Inbox {
        std::mutex mutex;
        /// 获取层解码产物(共享持有,进树时不再拷贝)。
        std::vector<std::pair<TileKey, std::shared_ptr<const Payload>>>
            decoded;
        /// worker 镶嵌产物(样式相关的派生物,进 readyMeshes_ 等 commit)。
        /// 带规则代次:setLayerRules 之后在途的任务用的是旧规则,回来时按
        /// 代次丢弃 —— 否则旧规则的网格会盖掉新规则刚镶好的。
        std::vector<std::tuple<TileKey, FeatureTileMesh, uint64_t>> meshes;
        std::vector<TileKey> failed;
    };

    void ingestInbox();

    /// 把矢量链的在途状态对账进 WorkLedger(每帧 update 末尾调,喂 gating 审计)。
    /// Landing = fetch/decode 在途(tree_.pendingCount)或 worker 镶嵌在途;
    /// Pumped  = 已镶好、等渲染线程 commit 的网格(readyMeshes_)。
    void syncWorkTickets();

    Options options_;
    Sinks sinks_;
    std::shared_ptr<MvtTileFetchCacheT<Payload, DecodeTraits>> tileCache_;
    std::shared_ptr<ThreadPool> decodePool_;
    ToFeaturesFn toFeatures_;

    UpdateStats lastStats_;
    VectorTileTreeT<Payload> tree_;
    /// 已 commit 到渲染层的瓦片(退出渲染集时 drop)。
    std::unordered_set<TileKey> activeTiles_;
    /// 已镶好但本帧未 commit 的网格(受 maxTileCommitsPerUpdate 限流)。
    /// 键在 renderTiles 里就留着等下一帧,不在就直接丢。
    std::unordered_map<TileKey, FeatureTileMesh> readyMeshes_;
    /// 已派出去、尚未回来的镶嵌任务(去重,防同一瓦片被反复派单)。
    std::unordered_set<TileKey> tessellating_;
    /// 样式层规则代次(setLayerRules 自增),用于丢弃在途的旧规则产物。
    uint64_t rulesEpoch_ = 0;

    std::shared_ptr<Inbox> inbox_;

    /// gating 账本对账槽(见 syncWorkTickets)。仅喂审计,当前零行为影响。
    WorkTicketSlot loadSlot_;    ///< Landing: fetch/decode + worker 镶嵌在途
    WorkTicketSlot commitSlot_;  ///< Pumped: 已镶好待 commit
};

/// MVT 载荷 → 引擎 Feature 列表的默认特质(层过滤 + 转换)。
/// 语义与旧的 MvtVectorSource worker 内联循环完全一致:
/// includeLayers 粗筛 → layerRules zoom/要素细则 → mvtLayerToFeatures。
struct MvtToFeatures {
    std::vector<Feature> operator()(
        const TileKey& key, std::shared_ptr<const MvtTile> tile,
        const std::vector<std::string>& includeLayers,
        const std::vector<SourceLayerRule>& rules) const {
        std::vector<Feature> features;
        for (const MvtLayer& layer : tile->layers) {
            if (!includeLayers.empty() &&
                std::find(includeLayers.begin(), includeLayers.end(),
                          layer.name) == includeLayers.end()) {
                continue;
            }
            // E2:层级规则。zoom 区间在**转换之前**判 —— 整层跳过时连
            // MVT→Feature 的转换都省了(P4 实测该转换 81ms/巨瓦),这正是
            // maplibre 把 layer minzoom/maxzoom 放在建桶前的理由。
            const SourceLayerRule* rule = nullptr;
            for (const SourceLayerRule& r : rules) {
                if (r.layer == layer.name) {
                    rule = &r;
                    break;
                }
            }
            if (rule &&
                (key.z < rule->minZoom || key.z > rule->maxZoom)) {
                continue;
            }
            for (Feature& f : mvtLayerToFeatures(layer, key)) {
                // 逐要素过滤在 worker 上跑(StyleFilter 不可变纯函数)。
                if (rule && rule->filter &&
                    !rule->filter->matches(
                        &f.properties, static_cast<double>(key.z))) {
                    continue;
                }
                features.push_back(std::move(f));
            }
        }
        return features;
    }
};

/// MVT 消费方别名:载荷 = 解码后的 MVT 瓦片。语义与旧的
/// `MvtVectorSource` 完全一致,调用方零改动。
using MvtVectorSource =
    VectorTileSourceT<MvtTile, MvtTileDecodeTraits, MvtToFeatures>;

// ==================== 模板实现(头文件可见,E3) ====================

template <typename Payload, typename DecodeTraits, typename ToFeaturesFn>
Rectangle VectorTileSourceT<Payload, DecodeTraits, ToFeaturesFn>::
    horizonViewRectangle(const Cartographic& cameraCarto,
                         double ellipsoidMinRadiusMeters) {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kHalfPi = 0.5 * kPi;
    // 两侧地平线角相加 = 相机与要素互见(要素侧上界 10km,同
    // FeatureRenderLayer::visibleBucketKeys)
    constexpr double kMaxFeatureAltitudeMeters = 10000.0;
    const double r = ellipsoidMinRadiusMeters;
    const double camAltitude = std::max(0.0, cameraCarto.height());
    double theta =
        std::acos(std::clamp(r / (r + camAltitude), 0.0, 1.0)) +
        std::acos(std::clamp(r / (r + kMaxFeatureAltitudeMeters), 0.0, 1.0));
    // 近场覆盖上限:地平线角里的"要素侧 10km"项在低空恒占 ~3.2°,
    // 不加盖低空视口矩形仍是数百 km → 防爆闸把 zoom 压回粗档,细档
    // (z14)永远进不来。上限取 ~4×视高的地面距离(斜视 45° 屏幕内
    // 主要地面范围),高空时该上限大于地平线角、自然失效;远场粗档
    // 环带覆盖属后续(v1 远景无底图可接受——细档下远处道路本就不可辨)。
    constexpr double kMinCoverageMeters = 2000.0;
    theta = std::min(
        theta, std::max(4.0 * camAltitude, kMinCoverageMeters) / r);

    double south = std::max(cameraCarto.latitude() - theta, -kHalfPi);
    double north = std::min(cameraCarto.latitude() + theta, kHalfPi);
    if (cameraCarto.latitude() - theta <= -kHalfPi ||
        cameraCarto.latitude() + theta >= kHalfPi) {
        // 圆覆盖极点:经度全量
        return Rectangle(-kPi, south, kPi, north);
    }
    // 球冠经度包围界:Δλ = asin(sinθ / cos(lat₀))
    const double halfLngSpan = std::asin(std::clamp(
        std::sin(theta) / std::cos(cameraCarto.latitude()), 0.0, 1.0));
    if (halfLngSpan * 2.0 >= 2.0 * kPi) {
        return Rectangle(-kPi, south, kPi, north);
    }
    // 折回 [-π, π];跨反经线时 west > east(树侧拆段消化)
    double west =
        std::remainder(cameraCarto.longitude() - halfLngSpan, 2.0 * kPi);
    double east =
        std::remainder(cameraCarto.longitude() + halfLngSpan, 2.0 * kPi);
    return Rectangle(west, south, east, north);
}

template <typename Payload, typename DecodeTraits, typename ToFeaturesFn>
void VectorTileSourceT<Payload, DecodeTraits, ToFeaturesFn>::update(
    const Rectangle& viewRect, double cameraHeightMeters) {
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    lastStats_ = UpdateStats{};
    const auto tIngest = Clock::now();
    ingestInbox();

    const auto tTree = Clock::now();
    lastStats_.ingestMs = ms(tIngest, tTree);
    typename VectorTileTreeT<Payload>::UpdateResult result =
        tree_.update(viewRect, cameraHeightMeters);
    const auto tDispatch = Clock::now();
    lastStats_.treeMs = ms(tTree, tDispatch);

    // 发缺瓦片请求(获取层单一化):fetch+解码+在途去重全在 tileCache_ ——
    // 与 drape/场共享实例时,同一块数据瓦跨消费方网络恰一次、解码恰一次。
    // 回调持 shared_ptr 收件箱,本对象析构后迟到安全;回调本体只做入箱
    // (mutex + push),无需再借 decodePool。树的 pending 集保证同一 key
    // 不重复走到这里,cache 的 inflight 兜跨消费方并发。
    if (tileCache_) {
        for (const TileKey& key : result.requestTiles) {
            std::weak_ptr<Inbox> weakInbox = inbox_;
            tileCache_->request(
                key,
                [key, weakInbox](std::shared_ptr<const Payload> tile) {
                    auto inbox = weakInbox.lock();
                    if (!inbox) return;
                    std::lock_guard<std::mutex> lock(inbox->mutex);
                    if (tile) {
                        inbox->decoded.emplace_back(key, std::move(tile));
                    } else {
                        inbox->failed.push_back(key);
                    }
                });
        }
    }

    // 已解码但还没网格的渲染瓦片 → 派 worker 镶嵌。持共享所有权,树的 LRU
    // 淘汰不会把 worker 脚下的数据抽走。
    for (const TileKey& key : result.renderTiles) {
        if (activeTiles_.count(key) || readyMeshes_.count(key) ||
            tessellating_.count(key)) {
            continue;
        }
        std::shared_ptr<const Payload> tile = tree_.loadedTileShared(key);
        if (!tile || !sinks_.tessellate) continue;
        tessellating_.insert(key);
        ++lastStats_.tessellateDispatched;
        std::weak_ptr<Inbox> weakInbox = inbox_;
        TessellateFn tessellate = sinks_.tessellate;
        std::vector<std::string> includeLayers = options_.includeLayers;
        std::vector<SourceLayerRule> rules = options_.layerRules;
        const uint64_t epoch = rulesEpoch_;
        ToFeaturesFn toFeatures = toFeatures_;
        auto work = [key, weakInbox, tile, tessellate, includeLayers, rules,
                     epoch, toFeatures]() {
            auto inbox = weakInbox.lock();
            if (!inbox) return;
            std::vector<Feature> features =
                toFeatures(key, tile, includeLayers, rules);
            FeatureTileMesh mesh = tessellate(key, std::move(features));
            std::lock_guard<std::mutex> lock(inbox->mutex);
            inbox->meshes.emplace_back(key, std::move(mesh), epoch);
        };
        if (decodePool_) decodePool_->enqueue(std::move(work));
        else work();
    }

    const auto tCommit = Clock::now();
    lastStats_.dispatchMs = ms(tDispatch, tCommit);

    // ---- R* 换手(2026-08-15,V24 根修消缺陷④)----
    //
    // renderTiles 是树给出的精确覆盖。旧差分"出集即 drop、进集限速 commit"
    // 会造成旧的瞬间没、新的慢慢来 —— 整片换代最坏 16 帧空窗。改为
    // **置换单元原子换手**:被替换的旧瓦(占位者)只在其替换内容全部
    // 就绪的那一次 update 里,与替换内容的 commit 同帧退场。过渡期宁可
    // 旧内容多留几帧,决不出现空窗;同理,替换内容不提前上屏(占位者
    // 还在时提前 commit = 祖先/子瓦同框重影)。
    std::unordered_set<TileKey> renderSet(result.renderTiles.begin(),
                                          result.renderTiles.end());
    auto isAncestorOf = [](const TileKey& a, const TileKey& b) {
        if (a.schemeId != b.schemeId || a.z >= b.z) return false;
        const int d = b.z - a.z;
        return (b.x >> d) == a.x && (b.y >> d) == a.y;
    };
    auto overlaps = [&](const TileKey& a, const TileKey& b) {
        return isAncestorOf(a, b) || isAncestorOf(b, a);
    };

    // 1) 出集分类:整片离开视口(渲染集里没有任何覆盖它的替换者)→ 立即
    //    drop;有替换者 → 占位者,等单元完备再换手。
    std::vector<TileKey> toDrop;
    std::vector<TileKey> occupants;
    for (const TileKey& key : activeTiles_) {
        if (renderSet.count(key)) continue;
        bool hasReplacement = false;
        for (const TileKey& r : result.renderTiles) {
            if (overlaps(r, key)) {
                hasReplacement = true;
                break;
            }
        }
        (hasReplacement ? occupants : toDrop).push_back(key);
    }
    for (const TileKey& key : toDrop) {
        if (sinks_.drop) sinks_.drop(key);
        activeTiles_.erase(key);
        ++lastStats_.drops;
    }
    // 已镶好但已不在渲染集的网格直接丢弃,别占内存等一个不会来的 commit。
    for (auto it = readyMeshes_.begin(); it != readyMeshes_.end();) {
        it = renderSet.count(it->first) ? std::next(it)
                                        : readyMeshes_.erase(it);
    }

    // 2) 置换单元换手:单元 = 占位者 + 渲染集中与其重叠的全部瓦。单元内
    //    所有瓦 active 或网格就绪才动手:commit 缺席者 + drop 占位者,
    //    同一次 update 完成。预算(maxTileCommitsPerUpdate)拦上传尖刺,
    //    但单元不可拆 —— 每帧至少放行一个完备单元,宁可单帧超预算
    //    (最坏 4^kMaxDescendantStandinLevels 块)也不把换手劈成两帧重影。
    const size_t budget = options_.maxTileCommitsPerUpdate;
    size_t commits = 0;
    std::sort(occupants.begin(), occupants.end(),
              [](const TileKey& a, const TileKey& b) {
                  if (a.z != b.z) return a.z < b.z;
                  if (a.y != b.y) return a.y < b.y;
                  return a.x < b.x;
              });
    for (const TileKey& occ : occupants) {
        if (budget != 0 && commits >= budget) break;
        std::vector<TileKey> unit;
        bool ready = true;
        for (const TileKey& r : result.renderTiles) {
            if (!overlaps(r, occ)) continue;
            if (activeTiles_.count(r)) continue;  // 已上屏(共享占位者)
            if (!readyMeshes_.count(r)) {
                ready = false;
                break;
            }
            unit.push_back(r);
        }
        if (!ready) continue;  // 占位者继续顶着,不空窗
        for (const TileKey& r : unit) {
            auto it = readyMeshes_.find(r);
            if (it == readyMeshes_.end()) continue;  // 同帧被前一单元提走
            if (sinks_.commit) sinks_.commit(r, std::move(it->second));
            readyMeshes_.erase(it);
            activeTiles_.insert(r);
            ++commits;
        }
        if (sinks_.drop) sinks_.drop(occ);
        activeTiles_.erase(occ);
        ++lastStats_.drops;
    }

    // 3) 无占位者区域的新上屏(初次进入/离开后返回),原节流语义。
    //    与仍在场的占位者重叠的瓦不得提前上屏(重影),由其单元统一换手。
    for (const TileKey& key : result.renderTiles) {
        if (activeTiles_.count(key)) continue;
        auto it = readyMeshes_.find(key);
        if (it == readyMeshes_.end()) continue;  // 还没镶好
        if (budget != 0 && commits >= budget) {
            break;  // 余下的留到下帧,renderTiles 稳定时差分是幂等的
        }
        bool blocked = false;
        for (const TileKey& occ : activeTiles_) {
            if (!renderSet.count(occ) && overlaps(occ, key)) {
                blocked = true;
                break;
            }
        }
        if (blocked) continue;
        if (sinks_.commit) sinks_.commit(key, std::move(it->second));
        readyMeshes_.erase(it);
        activeTiles_.insert(key);
        ++commits;
    }
    lastStats_.commits = static_cast<int>(commits);
    lastStats_.commitMs = ms(tCommit, Clock::now());

    syncWorkTickets();
}

template <typename Payload, typename DecodeTraits, typename ToFeaturesFn>
void VectorTileSourceT<Payload, DecodeTraits, ToFeaturesFn>::
    syncWorkTickets() {
    // Landing:fetch/decode 在途(tree_ 选中未到 = pending_)或 worker 镶嵌在途。
    // tree_.pendingCount 覆盖 request 派发→decode 回来这段窗口(此时 tessellating_
    // 与 readyMeshes_ 都空);ingest 与 dispatch 在同一次 update 内串行,无跨帧缝。
    const bool landing =
        tree_.pendingCount() > 0 || !tessellating_.empty();
    // Pumped:已镶好、等渲染线程 commit 的网格。停帧 = 永不 commit,故须出帧。
    const bool pumped = !readyMeshes_.empty();
    loadSlot_.reconcile(WorkLedger::Kind::Landing, "mvtVectorLoad", landing);
    commitSlot_.reconcile(WorkLedger::Kind::Pumped, "mvtVectorCommit", pumped);
}

template <typename Payload, typename DecodeTraits, typename ToFeaturesFn>
void VectorTileSourceT<Payload, DecodeTraits, ToFeaturesFn>::setLayerRules(
    std::vector<SourceLayerRule> rules) {
    options_.layerRules = std::move(rules);
    // 网格是样式相关的派生物 —— 规则变了旧网格就不再有效。全部丢弃,下一次
    // update 会按新规则重镶。解码结果留在树的 LRU,故重镶零重拉取。
    for (const TileKey& key : activeTiles_) {
        if (sinks_.drop) sinks_.drop(key);
    }
    activeTiles_.clear();
    readyMeshes_.clear();
    // 已在途的镶嵌任务用的是旧规则,回来时直接丢:tessellating_ 清空后,
    // ingestInbox 认不出它们,readyMeshes_ 会收下 —— 故这里不能只清
    // tessellating_,得让 ingest 侧也识别。用代次戳最省事。
    ++rulesEpoch_;
    tessellating_.clear();
}

template <typename Payload, typename DecodeTraits, typename ToFeaturesFn>
void VectorTileSourceT<Payload, DecodeTraits, ToFeaturesFn>::ingestInbox() {
    std::vector<std::pair<TileKey, std::shared_ptr<const Payload>>> decoded;
    std::vector<std::tuple<TileKey, FeatureTileMesh, uint64_t>> meshes;
    std::vector<TileKey> failed;
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        decoded.swap(inbox_->decoded);
        meshes.swap(inbox_->meshes);
        failed.swap(inbox_->failed);
    }
    for (auto& [key, tile] : decoded) {
        tree_.provideShared(key, std::move(tile));
    }
    for (auto& [key, mesh, epoch] : meshes) {
        tessellating_.erase(key);
        // 规则已变 → 这块是旧规则的产物,丢掉;下一次 update 会按新规则重派。
        if (epoch != rulesEpoch_) continue;
        readyMeshes_[key] = std::move(mesh);
    }
    for (const TileKey& key : failed) {
        tree_.markFailed(key);
    }
}

} // namespace earth_engine
