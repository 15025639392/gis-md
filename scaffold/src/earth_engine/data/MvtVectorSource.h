#pragma once

#include "FeatureStore.h"
#include "FeatureTileMesh.h"
#include "MvtDecoder.h"
#include "MvtFeatureConverter.h"
#include "MvtTileFetchCache.h"
#include "StyleFilter.h"
#include "VectorTileTree.h"
#include "../core/async/WorkLedger.h"
#include "../core/resources/SceneFrameResourceArbiter.h"
#include "../debug/PlatformLog.h"
#include "../tiling/TileKey.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <condition_variable>
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
    ~VectorTileSourceT() { waitForTessellationTasks(); }
    /// 获取层单一化(符号刀A.5):瓦片获取/解码/在途去重/LRU 全部经
    /// MvtTileFetchCacheT —— 与 drape(面)/路网场(线)共享同一实例时,
    /// 同一块数据瓦网络恰一次、解码恰一次、内存恰一份。本类不再自带
    /// fetch+decode 栈(旧 FetchFn 路径已删,测试用假 FetchFn 建 cache)。

    struct Options {
        typename VectorTileTreeT<Payload>::Options tree;
        /// 诊断名：仅用于 worker 慢任务分段日志。
        std::string debugName;
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
        /// 单个 source 同时排入共享 tessellation pool 的最大任务数
        /// (0 = 不限)。Amap 多 source 共用严格 FIFO 时用它做背压：最终
        /// 工作集不变，但 regions/main 不能一次把几十个任务灌在 POI 前面。
        size_t maxTessellationsInFlight = 0;
    };

    // Scene may host several independent sources behind the same MVT producer
    // grant. These per-call limits split that aggregate grant fairly without
    // turning a local instance share into a false global admission denial.
    // Standalone callers keep the historical unlimited behavior.
    struct FrameAdmissionLimits {
        uint32_t networkRequests = std::numeric_limits<uint32_t>::max();
        uint32_t workerDispatches = std::numeric_limits<uint32_t>::max();
        uint32_t gpuUploads = std::numeric_limits<uint32_t>::max();
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
    using CommitFn = std::function<TileMeshCommitResult(
        const TileKey&, FeatureTileMesh&)>;
    /// 渲染线程侧移除钩子(典型:FeatureRenderLayer::dropTileMesh)。
    using DropFn = std::function<void(const TileKey&)>;
    /// Render-thread hook that snapshots all layer-dependent tessellation
    /// state for one tile. The returned function is then safe to run on a
    /// worker without dereferencing the live layer.
    using PrepareTessellateFn = std::function<TessellateFn(const TileKey&)>;

    struct Sinks {
        TessellateFn tessellate;
        CommitFn commit;
        DropFn drop;
        PrepareTessellateFn prepareTessellate;
    };

    /// tileCache:获取层(fetch+解码+在途去重+LRU)。与其他消费方共享同一
    /// 实例即获得跨消费方去重;解码在 cache 的 fetch 回调线程完成。
    /// tessellationPool 为空则镶嵌在 cache 回调线程就地跑(仍不占渲染线程)。
    /// tessellationPool 用 shared_ptr:回调可能晚于宿主拆除 —— 回调里对 pool 只持
    /// weak,锁不上就丢弃工作。裸指针版真机崩过(tombstone_22:回调 enqueue
    /// 已析构线程池 = destroyed mutex abort)。
    VectorTileSourceT(Options options, Sinks sinks,
                      std::shared_ptr<MvtTileFetchCacheT<Payload, DecodeTraits>>
                          tileCache,
                      std::shared_ptr<ThreadPool> tessellationPool = nullptr,
                      ToFeaturesFn toFeatures = ToFeaturesFn{})
        : options_(std::move(options)),
          sinks_(std::move(sinks)),
          tileCache_(std::move(tileCache)),
          tessellationPool_(std::move(tessellationPool)),
          toFeatures_(std::move(toFeatures)),
          tree_(options_.tree),
          inbox_(std::make_shared<Inbox>()) {}

    /// 渲染线程每帧调用:驱动树、发缺瓦片请求、消化 worker 产物、
    /// 按渲染集差分 commit/drop 瓦片网格。
    void update(const Rectangle& viewRect, double cameraHeightMeters,
                SceneFrameResourceArbiter* resourceArbiter = nullptr,
                FrameAdmissionLimits frameLimits = {});

    /// 暂停不可见 Source，但继续排空异步收件箱和释放工作票据。
    /// 用于有样式 zoom 门控的粗层；直接跳过 update 会冻结 worker 结果。
    void suspend();

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
        int selectedZoom = 0;
        int64_t desiredTileCount = 0;
        size_t scannedTileCount = 0;
        size_t renderTileCount = 0;
        size_t requestTileCount = 0;
        size_t pendingTileCount = 0;
        size_t tessellatingTileCount = 0;
        size_t readyTileCount = 0;
        size_t activeTileCount = 0;
        size_t activeAncestorPairs = 0;
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
    const VectorTileTreeT<Payload>& tree() const { return tree_; }
    /// 已 commit 的瓦片数(诊断)。
    size_t activeTileCount() const { return activeTiles_.size(); }

    /// 是否还有瓦片在 worker 上镶嵌(产物尚未回到渲染线程)。帧级按需渲染
    /// 用:停帧会让 update() 不再被调用 → 收件箱永远没人排空 → 该瓦片
    /// 永远 commit 不上去,底图停在半成品且零报错。
    bool hasTessellationInFlight() const { return !tessellating_.empty(); }
    /// 已镶好、等待 commit 的瓦片数(诊断:持续 >0 说明上传闸偏紧)。
    size_t pendingCommitCount() const { return readyMeshes_.size(); }

    /// Wait until worker tessellation callbacks have stopped touching sinks.
    /// Scene uses this as the quiesce barrier before destroying the bound
    /// FeatureRenderLayer.  Fetch/decode callbacks only retain the inbox and
    /// are therefore safe after the source is gone; tessellation callbacks
    /// additionally invoke the caller-provided sink and must be drained.
    void waitForTessellationTasks() const {
        std::unique_lock<std::mutex> lock(lifecycleMutex_);
        lifecycleCv_.wait(lock, [this]() { return activeTessellationTasks_ == 0; });
    }

private:
    struct Inbox {
        std::mutex mutex;
        /// 获取层解码产物(共享持有,进树时不再拷贝)。
        std::vector<std::pair<TileKey, std::shared_ptr<const Payload>>>
            decoded;
        /// worker 镶嵌产物(样式相关的派生物,进 readyMeshes_ 等 commit)。
        struct MeshResult {
            TileKey key;
            FeatureTileMesh mesh;
            uint64_t rulesEpoch = 0;
            uint64_t viewEpoch = 0;
            uint64_t taskId = 0;
        };
        std::vector<MeshResult> meshes;
        struct MeshFailure {
            TileKey key;
            uint64_t rulesEpoch = 0;
            uint64_t viewEpoch = 0;
            uint64_t taskId = 0;
        };
        std::vector<MeshFailure> meshFailures;
        struct FailedTile {
            TileKey key;
            double retryNotBeforeMs = 0.0;
            bool retryable = false;
        };
        std::vector<FailedTile> failed;
    };

    void ingestTileInbox();
    void ingestMeshInbox(const std::unordered_set<TileKey>& renderSet);

    /// 把必须由渲染帧推进的矢量状态对账进 WorkLedger(update 末尾调)。
    /// fetch/decode 与 worker 镶嵌各自持有每任务 Landing 票；这里只同步
    /// 已镶好待 commit 与退避计时这两类 Pumped 工作。
    void syncWorkTickets();

    Options options_;
    Sinks sinks_;
    std::shared_ptr<MvtTileFetchCacheT<Payload, DecodeTraits>> tileCache_;
    std::shared_ptr<ThreadPool> tessellationPool_;
    ToFeaturesFn toFeatures_;

    UpdateStats lastStats_;
    VectorTileTreeT<Payload> tree_;
    /// 已 commit 到渲染层的瓦片(退出渲染集时 drop)。
    std::unordered_set<TileKey> activeTiles_;
    /// 已镶好但本帧未 commit 的网格(受 maxTileCommitsPerUpdate 限流)。
    /// 键在 renderTiles 里就留着等下一帧,不在就直接丢。
    std::unordered_map<TileKey, FeatureTileMesh> readyMeshes_;
    /// 已派出去、尚未回来的镶嵌任务(去重,防同一瓦片被反复派单)。
    std::unordered_map<TileKey, uint64_t> tessellating_;
    /// 样式层规则代次(setLayerRules 自增),用于丢弃在途的旧规则产物。
    uint64_t rulesEpoch_ = 0;
    uint64_t viewEpoch_ = 0;
    uint64_t nextTaskId_ = 1;
    std::unordered_set<TileKey> lastDesiredSet_;

    std::shared_ptr<Inbox> inbox_;

    // Number of worker callbacks that may still invoke sinks_.tessellate.
    // This is deliberately separate from tessellating_ (which is render-thread
    // bookkeeping and is cleared on epoch changes).  The counter provides a
    // real teardown barrier rather than relying on source destruction order.
    mutable std::mutex lifecycleMutex_;
    mutable std::condition_variable lifecycleCv_;
    size_t activeTessellationTasks_ = 0;

    /// 渲染线程必须持续推进的工作。fetch/decode 与 worker
    /// 镶嵌改为每任务 Landing 票，产物入 inbox 后在完成线程释放并唤醒。
    WorkTicketSlot commitSlot_;  ///< Pumped: 已镶好待 commit
    WorkTicketSlot retrySlot_;   ///< Pumped: 等待获取缓存退避截止时间
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
    const Rectangle& viewRect, double cameraHeightMeters,
    SceneFrameResourceArbiter* resourceArbiter,
    FrameAdmissionLimits frameLimits) {
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    lastStats_ = UpdateStats{};
    const auto tIngest = Clock::now();
    ingestTileInbox();

    const auto tTree = Clock::now();
    typename VectorTileTreeT<Payload>::UpdateResult result =
        tree_.update(viewRect, cameraHeightMeters);
    const auto tTreeDone = Clock::now();
    std::unordered_set<TileKey> renderSet(result.renderTiles.begin(),
                                          result.renderTiles.end());
    std::unordered_set<TileKey> desiredSet(result.desiredTiles.begin(),
                                           result.desiredTiles.end());
    if (desiredSet != lastDesiredSet_) {
        if (tileCache_) {
            for (const TileKey& oldKey : lastDesiredSet_) {
                if (!desiredSet.count(oldKey)) {
                    tileCache_->clearFailure(oldKey);
                }
            }
        }
        ++viewEpoch_;
        lastDesiredSet_ = std::move(desiredSet);
    }
    ingestMeshInbox(renderSet);
    const auto tDispatch = Clock::now();
    lastStats_.ingestMs =
        ms(tIngest, tTree) + ms(tTreeDone, tDispatch);
    lastStats_.treeMs = ms(tTree, tTreeDone);
    lastStats_.selectedZoom = result.selectedZoom;
    lastStats_.desiredTileCount = result.desiredTileCount;
    lastStats_.scannedTileCount = result.scannedTileCount;
    lastStats_.renderTileCount = result.renderTiles.size();
    lastStats_.requestTileCount = result.requestTiles.size();

    uint32_t networkRequests = 0;
    uint32_t workerDispatches = 0;
    uint32_t gpuUploads = 0;
    const auto tryAcquireFrameResource = [&resourceArbiter](
        SceneFrameResourceStage stage,
        uint32_t limit,
        uint32_t& used,
        uint32_t units = 1) {
        if (used > limit || units > limit - used) {
            if (resourceArbiter) {
                resourceArbiter->noteInstanceDeferredDemand(
                    SceneFrameResourceProducer::Mvt,
                    stage,
                    FrameResourcePriority::Normal,
                    units);
            }
            return false;
        }
        if (resourceArbiter &&
            !resourceArbiter->tryAcquire(
                SceneFrameResourceProducer::Mvt,
                stage,
                FrameResourcePriority::Normal,
                units)) {
            return false;
        }
        used += units;
        return true;
    };

    // 发缺瓦片请求(获取层单一化):fetch+解码+在途去重全在 tileCache_ ——
    // 与 drape/场共享实例时,同一块数据瓦跨消费方网络恰一次、解码恰一次。
    // 回调持 shared_ptr 收件箱,本对象析构后迟到安全;回调本体只做入箱
    // (mutex + push),无需再借 decodePool。树的 pending 集保证同一 key
    // 不重复走到这里,cache 的 inflight 兜跨消费方并发。
    if (tileCache_) {
        for (const TileKey& key : result.requestTiles) {
            if (!tryAcquireFrameResource(
                    SceneFrameResourceStage::NetworkRequest,
                    frameLimits.networkRequests,
                    networkRequests)) {
                tree_.deferRequest(key);
                continue;
            }
            std::weak_ptr<Inbox> weakInbox = inbox_;
            std::weak_ptr<MvtTileFetchCacheT<Payload, DecodeTraits>>
                weakTileCache = tileCache_;
            auto landingTicket = std::make_shared<WorkLedger::Ticket>(
                WorkLedger::shared().acquire(
                    WorkLedger::Kind::Landing, "mvtVectorFetch"));
            tileCache_->request(
                key,
                [key, weakInbox, weakTileCache, landingTicket](
                    std::shared_ptr<const Payload> tile) {
                    auto inbox = weakInbox.lock();
                    if (inbox) {
                        std::lock_guard<std::mutex> lock(inbox->mutex);
                        if (tile) {
                            inbox->decoded.emplace_back(key, std::move(tile));
                        } else {
                            const auto cache = weakTileCache.lock();
                            const auto failure = cache
                                ? cache->failureInfo(key)
                                : typename MvtTileFetchCacheT<
                                      Payload, DecodeTraits>::FailureInfo{};
                            inbox->failed.push_back(
                                {key, failure.retryNotBeforeMs,
                                 failure.retryable});
                        }
                    }
                    // 先入箱再释放：Landing 唤醒后的那一帧必须已经
                    // 能看到产物。每请求一张票也避免 aggregate slot 在
                    // callback/update 并发时“先释放、后误重领”的睡死窗口。
                    landingTicket->release();
                });
        }
    }

    // 已解码但还没网格的渲染瓦片 → 派 worker 镶嵌。持共享所有权,树的 LRU
    // 淘汰不会把 worker 脚下的数据抽走。
    for (const TileKey& key : result.renderTiles) {
        if (options_.maxTessellationsInFlight != 0 &&
            tessellating_.size() >= options_.maxTessellationsInFlight) {
            break;
        }
        if (activeTiles_.count(key) || readyMeshes_.count(key) ||
            tessellating_.count(key)) {
            continue;
        }
        std::shared_ptr<const Payload> tile = tree_.loadedTileShared(key);
        TessellateFn tessellate = sinks_.prepareTessellate
            ? sinks_.prepareTessellate(key)
            : sinks_.tessellate;
        if (!tile || !tessellate) continue;
        if (!tryAcquireFrameResource(
                SceneFrameResourceStage::WorkerDispatch,
                frameLimits.workerDispatches,
                workerDispatches)) {
            continue;
        }
        ++lastStats_.tessellateDispatched;
        std::weak_ptr<Inbox> weakInbox = inbox_;
        std::vector<std::string> includeLayers = options_.includeLayers;
        std::vector<SourceLayerRule> rules = options_.layerRules;
        const uint64_t rulesEpoch = rulesEpoch_;
        const uint64_t viewEpoch = viewEpoch_;
        const uint64_t taskId = nextTaskId_++;
        tessellating_[key] = taskId;
        ToFeaturesFn toFeatures = toFeatures_;
        std::string debugName = options_.debugName;
        auto landingTicket = std::make_shared<WorkLedger::Ticket>(
            WorkLedger::shared().acquire(
                WorkLedger::Kind::Landing, "mvtVectorTessellate"));
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            ++activeTessellationTasks_;
        }
        auto work = [this, key, weakInbox, tile, tessellate, includeLayers, rules,
                     rulesEpoch, viewEpoch, taskId, toFeatures, debugName,
                     landingTicket]() {
            auto finishTask = [this]() {
                std::lock_guard<std::mutex> lock(lifecycleMutex_);
                if (activeTessellationTasks_ > 0) --activeTessellationTasks_;
                lifecycleCv_.notify_all();
            };
            auto inbox = weakInbox.lock();
            if (!inbox) {
                landingTicket->release();
                finishTask();
                return;
            }
            try {
                const auto convertStart = Clock::now();
                std::vector<Feature> features =
                    toFeatures(key, tile, includeLayers, rules);
                const auto tessStart = Clock::now();
                const size_t featureCount = features.size();
                FeatureTileMesh mesh = tessellate(key, std::move(features));
                const auto taskEnd = Clock::now();
                const double convertMs =
                    std::chrono::duration<double, std::milli>(
                        tessStart - convertStart).count();
                const double tessMs =
                    std::chrono::duration<double, std::milli>(
                        taskEnd - tessStart).count();
                if (!debugName.empty() &&
                    (convertMs + tessMs >= 100.0 ||
                     mesh.diagnostics.rejectedFeatures != 0)) {
                    const auto& rejectionCounts =
                        mesh.diagnostics.rejectionCounts;
                    uint8_t rejectTopGeometry = 0;
                    int rejectTopClass = 0;
                    int rejectTopSubKey = 0;
                    size_t rejectTopCount = 0;
                    for (const auto& [identity, count] :
                         mesh.diagnostics.rejectedIdentities) {
                        if (count <= rejectTopCount) continue;
                        rejectTopGeometry = std::get<0>(identity);
                        rejectTopClass = std::get<1>(identity);
                        rejectTopSubKey = std::get<2>(identity);
                        rejectTopCount = count;
                    }
                    platformLog(
                        LogLevel::Info, "VectorTessSlow",
                        "%s z=%d x=%d y=%d features=%zu convert=%.2fms "
                        "tess=%.2fms admit=%.2fms polygon=%.2fms line=%.2fms "
                        "polySetup=%.2fms polyDensify=%.2fms "
                        "polyIntersect=%.2fms polyCdt=%.2fms polyEcef=%.2fms "
                        "cdtSuper=%.2fms cdtPoint=%.2fms "
                        "cdtConstraint=%.2fms cdtExtract=%.2fms "
                        "extrude=%.2fms symbol=%.2fms accepted=%zu rejected=%zu "
                        "polyN=%zu lineN=%zu extrudeN=%zu symbolN=%zu "
                        "rings=%zu points=%zu slowest=%.2fms class=%d sub=%d "
                        "slowRings=%zu slowPoints=%zu polyInput=%zu "
                        "polyDense=%zu polyConstraints=%zu/%zu "
                        "polyPairs=%zu/%zu polyTris=%zu "
                        "cdtPointTests=%zu cdtBad=%zu cdtEdgeLookups=%zu "
                        "cdtCrossTests=%zu cdtConstraints=%zu/%zu "
                        "cdtPeakTris=%zu cdtCapacityGrowths=%zu/%zu "
                        "rejectReasons=%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu/%zu "
                        "rejectTop=%u:%d:%d/%zu",
                        debugName.c_str(), key.z, key.x, key.y, featureCount,
                        convertMs, tessMs, mesh.diagnostics.admissionMs,
                        mesh.diagnostics.polygonMs, mesh.diagnostics.lineMs,
                        mesh.diagnostics.polygonSetupMs,
                        mesh.diagnostics.polygonDensifyMs,
                        mesh.diagnostics.polygonIntersectionMs,
                        mesh.diagnostics.polygonCdtMs,
                        mesh.diagnostics.polygonEcefMs,
                        mesh.diagnostics.polygonCdtSuperMs,
                        mesh.diagnostics.polygonCdtPointMs,
                        mesh.diagnostics.polygonCdtConstraintMs,
                        mesh.diagnostics.polygonCdtExtractMs,
                        mesh.diagnostics.extrusionMs, mesh.diagnostics.symbolMs,
                        mesh.diagnostics.admittedFeatures,
                        mesh.diagnostics.rejectedFeatures,
                        mesh.diagnostics.polygonFeatures,
                        mesh.diagnostics.lineFeatures,
                        mesh.diagnostics.extrusionFeatures,
                        mesh.diagnostics.symbolFeatures,
                        mesh.diagnostics.rings, mesh.diagnostics.points,
                        mesh.diagnostics.slowestFeatureMs,
                        mesh.diagnostics.slowestClassCode,
                        mesh.diagnostics.slowestSubKey,
                        mesh.diagnostics.slowestRings,
                        mesh.diagnostics.slowestPoints,
                        mesh.diagnostics.polygonInputPoints,
                        mesh.diagnostics.polygonDensifiedPoints,
                        mesh.diagnostics.polygonInitialConstraints,
                        mesh.diagnostics.polygonFinalConstraints,
                        mesh.diagnostics.polygonIntersectionCandidatePairs,
                        mesh.diagnostics.polygonIntersectionPairs,
                        mesh.diagnostics.polygonTriangles,
                        mesh.diagnostics.polygonCdtPointTriangleTests,
                        mesh.diagnostics.polygonCdtPointBadTriangles,
                        mesh.diagnostics.polygonCdtConstraintEdgeTests,
                        mesh.diagnostics.polygonCdtConstraintCrossTests,
                        mesh.diagnostics.polygonCdtConstraintsAlreadyPresent,
                        mesh.diagnostics.polygonCdtConstraintsInserted,
                        mesh.diagnostics.polygonCdtPeakTriangles,
                        mesh.diagnostics.polygonCdtPointCapacityGrowths,
                        mesh.diagnostics.polygonCdtTriangleCapacityGrowths,
                        rejectionCounts[0], rejectionCounts[1],
                        rejectionCounts[2], rejectionCounts[3],
                        rejectionCounts[4], rejectionCounts[5],
                        rejectionCounts[6], rejectionCounts[7],
                        rejectionCounts[8], rejectTopGeometry,
                        rejectTopClass, rejectTopSubKey, rejectTopCount);
                }
                {
                    std::lock_guard<std::mutex> lock(inbox->mutex);
                    inbox->meshes.push_back(typename Inbox::MeshResult{
                        key, std::move(mesh), rulesEpoch, viewEpoch, taskId});
                }
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(inbox->mutex);
                    inbox->meshFailures.push_back(typename Inbox::MeshFailure{
                        key, rulesEpoch, viewEpoch, taskId});
                }
            }
            // 与 fetch 同契约：成功/异常都在结果入箱后释放，
            // 渲染线程无需轮询 worker，但不会错过完成唤醒。
            landingTicket->release();
            finishTask();
        };
        try {
            if (tessellationPool_) tessellationPool_->enqueue(std::move(work));
            else work();
        } catch (...) {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            if (activeTessellationTasks_ > 0) --activeTessellationTasks_;
            lifecycleCv_.notify_all();
            throw;
        }
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
        // A replacement unit is an atomic visibility transaction. Its cost is
        // one arbitration unit (not one child tile): charging unit.size()
        // makes a valid 4^k descendant swap permanently impossible when the
        // frame GPU grant is smaller than the transaction. The local commit
        // cap still counts actual uploaded tiles for burst control.
        if (!unit.empty() &&
            !tryAcquireFrameResource(
                SceneFrameResourceStage::GpuUpload,
                frameLimits.gpuUploads,
                gpuUploads)) {
            continue;  // 原子置换单元不可拆；旧占位者继续上屏
        }
        std::vector<TileKey> committedInUnit;
        for (const TileKey& r : unit) {
            auto it = readyMeshes_.find(r);
            if (it == readyMeshes_.end()) continue;  // 同帧被前一单元提走
            if (!sinks_.commit) continue;
            const TileMeshCommitResult commitResult =
                sinks_.commit(r, it->second);
            if (commitResult == TileMeshCommitResult::RetryableFailure) {
                ready = false;
                break;
            }
            readyMeshes_.erase(it);
            activeTiles_.insert(r);
            committedInUnit.push_back(r);
            ++commits;
        }
        if (!ready) {
            // GPU commit 不是可回滚事务；若单元中的后续瓦片上传失败，
            // 立即撤销本单元已成功上传的替换者，保留旧占位者，避免父子
            // 同框。失败及尚未尝试的瓦仍留在 readyMeshes_；已经 commit
            // 成功的瓦可能已被 sink 消费 CPU mesh，故 drop 后由下一帧从
            // loaded tile 重新 tessellate。Retryable GPU failure 很少见，
            // 这里选择稀有失败时重算，避免每次正常 LOD 换手都复制整份 mesh。
            for (const TileKey& committed : committedInUnit) {
                if (sinks_.drop) sinks_.drop(committed);
                activeTiles_.erase(committed);
                ++lastStats_.drops;
            }
            continue;
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
        if (!sinks_.commit) continue;
        if (!tryAcquireFrameResource(
                SceneFrameResourceStage::GpuUpload,
                frameLimits.gpuUploads,
                gpuUploads)) {
            break;
        }
        const TileMeshCommitResult commitResult =
            sinks_.commit(key, it->second);
        if (commitResult == TileMeshCommitResult::RetryableFailure) continue;
        readyMeshes_.erase(it);
        activeTiles_.insert(key);
        ++commits;
    }
    lastStats_.commits = static_cast<int>(commits);
    lastStats_.commitMs = ms(tCommit, Clock::now());
    lastStats_.pendingTileCount = tree_.pendingCount();
    lastStats_.tessellatingTileCount = tessellating_.size();
    lastStats_.readyTileCount = readyMeshes_.size();
    lastStats_.activeTileCount = activeTiles_.size();
    for (const TileKey& a : activeTiles_) {
        for (const TileKey& b : activeTiles_) {
            if (isAncestorOf(a, b)) ++lastStats_.activeAncestorPairs;
        }
    }

    syncWorkTickets();
}

template <typename Payload, typename DecodeTraits, typename ToFeaturesFn>
void VectorTileSourceT<Payload, DecodeTraits, ToFeaturesFn>::
    syncWorkTickets() {
    // Pumped:已镶好、等渲染线程 commit 的网格。停帧 = 永不 commit,故须出帧。
    const bool pumped = !readyMeshes_.empty();
    commitSlot_.reconcile(WorkLedger::Kind::Pumped, "mvtVectorCommit", pumped);
    // 退避截止时间本身不是 Landing（没有网络/worker 会自然释放令牌），
    // 但按需渲染若此时入睡就永远不会再调用 update。短暂持有独立 Pumped
    // 令牌，直到下一帧释放 retryNotBefore_ 并重新发起请求；缓存自身仍
    // 决定网络退避和最多两次重试，不会因这里保帧而产生网络风暴。
    retrySlot_.reconcile(WorkLedger::Kind::Pumped, "mvtVectorRetry",
                         tree_.hasRetryPending());
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
void VectorTileSourceT<Payload, DecodeTraits, ToFeaturesFn>::suspend() {
    // suspend 是本帧的真实状态，不应继续暴露上一次 update 的瓦片数与
    // 档位；否则远景门控掉 z12 水系后，诊断日志仍会看起来像它在活动。
    lastStats_ = UpdateStats{};
    // Stop the producer side first, then wait until no worker can dereference
    // the sink-bound layer.  Only after this barrier is it safe to drop GPU
    // meshes and let Scene destroy the layer.
    waitForTessellationTasks();
    ingestTileInbox();
    ++viewEpoch_;
    lastDesiredSet_.clear();
    tessellating_.clear();
    const std::unordered_set<TileKey> emptyRenderSet;
    ingestMeshInbox(emptyRenderSet);
    readyMeshes_.clear();
    for (const TileKey& key : activeTiles_) {
        if (sinks_.drop) sinks_.drop(key);
    }
    activeTiles_.clear();
    syncWorkTickets();
    lastStats_.pendingTileCount = tree_.pendingCount();
    lastStats_.tessellatingTileCount = tessellating_.size();
    lastStats_.readyTileCount = readyMeshes_.size();
    lastStats_.activeTileCount = activeTiles_.size();
}

template <typename Payload, typename DecodeTraits, typename ToFeaturesFn>
void VectorTileSourceT<Payload, DecodeTraits, ToFeaturesFn>::ingestTileInbox() {
    std::vector<std::pair<TileKey, std::shared_ptr<const Payload>>> decoded;
    std::vector<typename Inbox::FailedTile> failed;
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        decoded.swap(inbox_->decoded);
        failed.swap(inbox_->failed);
    }
    for (auto& [key, tile] : decoded) {
        tree_.provideShared(key, std::move(tile));
    }
    for (const auto& failure : failed) {
        if (failure.retryable) {
            tree_.markFailedUntil(failure.key, failure.retryNotBeforeMs);
        } else {
            tree_.markFailed(failure.key);
        }
    }
}

template <typename Payload, typename DecodeTraits, typename ToFeaturesFn>
void VectorTileSourceT<Payload, DecodeTraits, ToFeaturesFn>::ingestMeshInbox(
    const std::unordered_set<TileKey>& renderSet) {
    std::vector<typename Inbox::MeshResult> meshes;
    std::vector<typename Inbox::MeshFailure> failures;
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        meshes.swap(inbox_->meshes);
        failures.swap(inbox_->meshFailures);
    }
    auto finishTask = [&](const TileKey& key, uint64_t taskId) {
        auto it = tessellating_.find(key);
        if (it != tessellating_.end() && it->second == taskId) {
            tessellating_.erase(it);
        }
    };
    for (auto& result : meshes) {
        finishTask(result.key, result.taskId);
        if (result.rulesEpoch != rulesEpoch_ ||
            result.viewEpoch != viewEpoch_ ||
            !renderSet.count(result.key)) {
            continue;
        }
        readyMeshes_[result.key] = std::move(result.mesh);
    }
    for (const auto& failure : failures) {
        finishTask(failure.key, failure.taskId);
        // worker 异常不污染已解码 tile；清掉在途状态后，只要 key 仍是
        // 当前渲染集，下一次 update 会重新派发镶嵌。
    }
}

} // namespace earth_engine
