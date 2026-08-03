#pragma once

#include "FeatureStore.h"
#include "MvtDecoder.h"
#include "VectorTileTree.h"
#include "../tiling/TileKey.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

class ThreadPool;

/// 只读 MVT 底图源(P4c):VectorTileTree 选择 + 拉取/解码调度 +
/// 要素灌注专用 FeatureStore,渲染由外挂既有 FeatureRenderLayer 完成
/// ("两种源一条下游",设计 §4——fill/line/point/label/stencil/样式
/// 表达式/snap 全部复用,MVT 侧零渲染代码)。
///
/// store 语义:目标 store 由外部注入(FeatureRenderLayer 自有其
/// store,直接灌它,渲染零接线),本类只保证其中**当前渲染瓦片集**
/// 的要素。瓦片进入 renderTiles 时转换激活入 store,退出时整瓦片
/// 移除——多 zoom 的 LRU 缓存瓦片若常驻 store 会永久叠画(z8 路网
/// 压 z12 路网)。解码后的 MvtTile 缓存归树的 LRU,重新激活只重
/// 转换、零重拉取/重解码。祖先回退期的粗细并存是加载暂态,与
/// maplibre 行为一致。注入的 store 应专用于本源(update 只移除自己
/// 灌入的 ID,但混入他源要素会破坏"只存渲染集"的叠画契约)。
///
/// 线程契约:update() 必须在渲染线程调用(store 写入约定同
/// FeatureRenderLayer);fetch 回调/解码任意线程,结果经收件箱在
/// update() 内消化。析构后迟到的回调安全(shared_ptr 收件箱)。
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
        /// 单次 update 的激活要素预算(0 = 不限)。store 写入会触发
        /// FeatureRenderLayer 同步重镶(镶嵌 worker 化未做),整视口
        /// 瓦片同帧激活会把渲染线程钉死分钟级(真机实测)——分帧摊销
        /// 是结构必需不是调优。每帧至少激活一整瓦片(瓦片是激活原子,
        /// 部分激活会让"渲染集=store 内容"契约碎掉)。
        size_t maxActivationFeaturesPerUpdate = 800;
    };

    /// decodePool 为空则在 fetch 回调线程就地解码(仍不占渲染线程)。
    /// store 生命周期须覆盖本对象(典型:FeatureRenderLayer::store())。
    MvtVectorSource(Options options, FeatureStore& store, FetchFn fetch,
                    ThreadPool* decodePool = nullptr);

    /// 渲染线程每帧调用:驱动树、发缺瓦片请求、消化解码结果、
    /// 按渲染集差分激活/移除 store 要素。
    void update(const Rectangle& viewRect, double cameraHeightMeters);

    /// 相机地平线圆的经纬包围矩形(视口 viewRect 的标准来源;数学与
    /// FeatureRenderLayer::visibleBucketKeys 同源:两侧地平线角相加 +
    /// 球冠经度包围界)。跨反经线时返回 west > east 的跨界矩形,
    /// update/VectorTileTree 原生消化。
    static Rectangle horizonViewRectangle(const Cartographic& cameraCarto,
                                          double ellipsoidMinRadiusMeters);

    FeatureStore& store() { return store_; }
    const FeatureStore& store() const { return store_; }

    VectorTileTree& tree() { return tree_; }
    size_t activeTileCount() const { return activeTiles_.size(); }

private:
    struct Inbox {
        std::mutex mutex;
        std::vector<std::pair<TileKey, MvtTile>> decoded;
        std::vector<TileKey> failed;
    };

    void ingestInbox();
    /// 返回该瓦片实际灌入的要素数(计入激活预算)。
    size_t activateTile(const TileKey& key);
    void deactivateTile(const TileKey& key);

    Options options_;
    FetchFn fetch_;
    ThreadPool* decodePool_ = nullptr;

    VectorTileTree tree_;
    FeatureStore& store_;
    /// 激活瓦片 → 灌入 store 的要素 ID(退出渲染集时整批移除)。
    std::unordered_map<TileKey, std::vector<FeatureId>> activeTiles_;

    std::shared_ptr<Inbox> inbox_;
};

} // namespace earth_engine
