#pragma once

#include "FeatureStore.h"
#include "FeatureTileMesh.h"
#include "MvtDecoder.h"
#include "MvtTileFetchCache.h"
#include "StyleFilter.h"
#include "VectorTileTree.h"
#include "../tiling/TileKey.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <tuple>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace earth_engine {

class ThreadPool;

/// 只读 MVT 底图源(P4c):VectorTileTree 选择 + 拉取/解码调度 +
/// 要素灌注专用 FeatureStore,渲染由外挂既有 FeatureRenderLayer 完成
/// ("两种源一条下游",设计 §4——fill/line/point/label/stencil/样式
/// 表达式/snap 全部复用,MVT 侧零渲染代码)。
///
/// **E1:瓦片即桶,worker 全链镶嵌。** 拉取 → 解码 → **镶嵌**全在 worker
/// 完成,渲染线程只做「上传 + 整瓦原子替换」。相比原先灌 FeatureStore 的
/// 做法,这条通路没有 store、没有脏桶差分、没有激活预算 —— 那三样都是为了
/// 让「整视口要素一次性写进空间分桶 store」不把渲染线程钉死而生的补丁
/// (P4 真机实测 debug 下 16s/帧),根因是拿编辑用的差分结构承载只读底图。
///
/// 渲染集语义不变:瓦片进入 renderTiles 时 commit 网格,退出时 drop ——
/// 多 zoom 的 LRU 缓存瓦片若常驻会永久叠画(z8 路网压 z12 路网)。解码后
/// 的 MvtTile 缓存归树的 LRU,重新激活只重镶嵌、零重拉取/重解码。祖先回退
/// 期的粗细并存是加载暂态,与 maplibre 行为一致。
///
/// 线程契约:update() 必须在渲染线程调用(commit/drop 要 GL 上下文);
/// fetch 回调/解码/镶嵌任意线程,结果经收件箱在 update() 内消化。析构后
/// 迟到的回调安全(shared_ptr 收件箱)。
class MvtVectorSource {
public:
    /// 获取层单一化(符号刀A.5):瓦片获取/解码/在途去重/LRU 全部经
    /// MvtTileFetchCache —— 与 drape(面)/路网场(线)共享同一实例时,
    /// 同一块数据瓦网络恰一次、解码恰一次、内存恰一份。本类不再自带
    /// fetch+decode 栈(旧 FetchFn 路径已删,测试用假 FetchFn 建 cache)。

    struct Options {
        VectorTileTree::Options tree;
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
    MvtVectorSource(Options options, Sinks sinks,
                    std::shared_ptr<MvtTileFetchCache> tileCache,
                    std::shared_ptr<ThreadPool> decodePool = nullptr);

    /// 渲染线程每帧调用:驱动树、发缺瓦片请求、消化 worker 产物、
    /// 按渲染集差分 commit/drop 瓦片网格。
    void update(const Rectangle& viewRect, double cameraHeightMeters);

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

    VectorTileTree& tree() { return tree_; }
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
        std::vector<std::pair<TileKey, std::shared_ptr<const MvtTile>>> decoded;
        /// worker 镶嵌产物(样式相关的派生物,进 readyMeshes_ 等 commit)。
        /// 带规则代次:setLayerRules 之后在途的任务用的是旧规则,回来时按
        /// 代次丢弃 —— 否则旧规则的网格会盖掉新规则刚镶好的。
        std::vector<std::tuple<TileKey, FeatureTileMesh, uint64_t>> meshes;
        std::vector<TileKey> failed;
    };

    void ingestInbox();

    Options options_;
    Sinks sinks_;
    std::shared_ptr<MvtTileFetchCache> tileCache_;
    std::shared_ptr<ThreadPool> decodePool_;

    VectorTileTree tree_;
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
};

} // namespace earth_engine
