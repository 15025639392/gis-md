#pragma once

#include "FeatureStore.h"
#include "FeatureTileMesh.h"
#include "MvtDecoder.h"
#include "VectorTileTree.h"
#include "../tiling/TileKey.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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
    /// (statusCode, body):statusCode==200 且 body 非空视为成功。
    using FetchCallback = std::function<void(int, std::vector<uint8_t>)>;
    /// 由调用方注入网络实现(P4c demo 接 CurlScheduler;测试注入假源)。
    using FetchFn = std::function<void(const TileKey&, FetchCallback)>;

    struct Options {
        VectorTileTree::Options tree;
        /// 只导入这些源图层;空 = 全部。
        std::vector<std::string> includeLayers;
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
    using TessellateFn =
        std::function<FeatureTileMesh(std::vector<Feature>&&)>;
    /// 渲染线程侧网格落地钩子(典型:FeatureRenderLayer::commitTileMesh)。
    using CommitFn = std::function<void(const TileKey&, FeatureTileMesh&&)>;
    /// 渲染线程侧移除钩子(典型:FeatureRenderLayer::dropTileMesh)。
    using DropFn = std::function<void(const TileKey&)>;

    struct Sinks {
        TessellateFn tessellate;
        CommitFn commit;
        DropFn drop;
    };

    /// decodePool 为空则在 fetch 回调线程就地解码+镶嵌(仍不占渲染线程)。
    MvtVectorSource(Options options, Sinks sinks, FetchFn fetch,
                    ThreadPool* decodePool = nullptr);

    /// 渲染线程每帧调用:驱动树、发缺瓦片请求、消化 worker 产物、
    /// 按渲染集差分 commit/drop 瓦片网格。
    void update(const Rectangle& viewRect, double cameraHeightMeters);

    /// 相机地平线圆的经纬包围矩形(视口 viewRect 的标准来源;数学与
    /// FeatureRenderLayer::visibleBucketKeys 同源:两侧地平线角相加 +
    /// 球冠经度包围界)。跨反经线时返回 west > east 的跨界矩形,
    /// update/VectorTileTree 原生消化。
    static Rectangle horizonViewRectangle(const Cartographic& cameraCarto,
                                          double ellipsoidMinRadiusMeters);

    VectorTileTree& tree() { return tree_; }
    /// 已 commit 的瓦片数(诊断)。
    size_t activeTileCount() const { return activeTiles_.size(); }
    /// 已镶好、等待 commit 的瓦片数(诊断:持续 >0 说明上传闸偏紧)。
    size_t pendingCommitCount() const { return readyMeshes_.size(); }

private:
    struct Inbox {
        std::mutex mutex;
        /// worker 解码产物(样式无关,进树的 LRU 缓存)。
        std::vector<std::pair<TileKey, MvtTile>> decoded;
        /// worker 镶嵌产物(样式相关的派生物,进 readyMeshes_ 等 commit)。
        std::vector<std::pair<TileKey, FeatureTileMesh>> meshes;
        std::vector<TileKey> failed;
    };

    void ingestInbox();

    Options options_;
    Sinks sinks_;
    FetchFn fetch_;
    ThreadPool* decodePool_ = nullptr;

    VectorTileTree tree_;
    /// 已 commit 到渲染层的瓦片(退出渲染集时 drop)。
    std::unordered_set<TileKey> activeTiles_;
    /// 已镶好但本帧未 commit 的网格(受 maxTileCommitsPerUpdate 限流)。
    /// 键在 renderTiles 里就留着等下一帧,不在就直接丢。
    std::unordered_map<TileKey, FeatureTileMesh> readyMeshes_;
    /// 已派出去、尚未回来的镶嵌任务(去重,防同一瓦片被反复派单)。
    std::unordered_set<TileKey> tessellating_;

    std::shared_ptr<Inbox> inbox_;
};

} // namespace earth_engine
