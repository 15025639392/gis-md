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
/// store 语义:只保存**当前渲染瓦片集**的要素。瓦片进入 renderTiles
/// 时转换激活入 store,退出时整瓦片移除——多 zoom 的 LRU 缓存瓦片若
/// 常驻 store 会永久叠画(z8 路网压 z12 路网)。解码后的 MvtTile 缓存
/// 归树的 LRU,重新激活只重转换、零重拉取/重解码。祖先回退期的粗细
/// 并存是加载暂态,与 maplibre 行为一致。
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
    };

    /// decodePool 为空则在 fetch 回调线程就地解码(仍不占渲染线程)。
    MvtVectorSource(Options options, FetchFn fetch,
                    ThreadPool* decodePool = nullptr);

    /// 渲染线程每帧调用:驱动树、发缺瓦片请求、消化解码结果、
    /// 按渲染集差分激活/移除 store 要素。
    void update(const Rectangle& viewRect, double cameraHeightMeters);

    /// 渲染/拾取/snap 挂这个 store(外部只读;写入归本类)。
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
    void activateTile(const TileKey& key);
    void deactivateTile(const TileKey& key);

    Options options_;
    FetchFn fetch_;
    ThreadPool* decodePool_ = nullptr;

    VectorTileTree tree_;
    FeatureStore store_;
    /// 激活瓦片 → 灌入 store 的要素 ID(退出渲染集时整批移除)。
    std::unordered_map<TileKey, std::vector<FeatureId>> activeTiles_;

    std::shared_ptr<Inbox> inbox_;
};

} // namespace earth_engine
