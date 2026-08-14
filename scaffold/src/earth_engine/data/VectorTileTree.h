#pragma once

#include "MvtDecoder.h"
#include "../core/math/Rectangle.h"
#include "../tiling/TileKey.h"
#include "../tiling/TileScheme.h"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace earth_engine {

/// 只读 MVT 底图的瓦片树(P4b,设计 §5)。
///
/// 与地形 Tileset 刻意分离(独立实例、独立 zoom 范围/缓存,LOD 语义
/// 不互污;TileContentLoadResult 是 glTF/地形耦合的,矢量内容不塞进去)。
/// 选择模型(R*,2026-08-15 V24 根修):视高定目标 zoom → **置换式细化**
/// —— 全有全无的祖先/后代回退,renderTiles 恒为视口内**精确覆盖**
/// (无重叠、有存货即无空洞)。语义对拍地形 Tileset 的 replacement
/// refinement 与 maplibre _updateRetainedTiles,但比 maplibre 多"无重叠"
/// 保证:我们是世界空间渲染、无逐瓦 stencil,祖先与子瓦同框即要素重影。
/// 不做地形式逐瓦片 SSE 细分。
///
/// overzoom 与 maplibre 的差异(刻意):maplibre 在瓦片裁剪空间逐瓦片
/// 渲染,超 maxZoom 必须把父瓦片几何 scale+offset+clip 切片;本引擎把
/// 要素转成世界空间几何渲染,maxZoom 瓦片的几何天然连续覆盖任意深的
/// 缩放,故 overzoom = 目标 zoom 钳制到 maxZoom,零切片。
///
/// 纯选择/缓存逻辑,不发网络请求:update 产出 requestTiles,由调用方
/// (P4c)拉取后经 provide()/markFailed() 回灌。非线程安全,调用方
/// 保证单线程访问(与 FeatureStore 同约定)。
class VectorTileTree {
public:
    struct Options {
        int minZoom = 0;
        int maxZoom = 14;
        /// LRU 缓存瓦片数上限(设计 §5:移动端保守 200–400)。
        /// 本帧在渲染的瓦片不计入淘汰。
        size_t maxCachedTiles = 300;
        /// 单次 update 的 desired 瓦片数上限;超出自动降 zoom 重枚举
        /// (掠视地平线视口在高 zoom 会枚举出海量瓦片,必须有闸)。
        int maxTilesPerView = 64;
        /// 目标 zoom 偏置(正 = 更细)。
        double zoomBias = 0.0;
    };

    struct UpdateResult {
        /// 应渲染的已加载瓦片(含祖先回退,已去重;先粗后细)。
        std::vector<TileKey> renderTiles;
        /// 需发起请求的瓦片(视口中心优先排序;已登记 pending,
        /// 调用方必须最终以 provide()/markFailed() 回应)。
        std::vector<TileKey> requestTiles;
    };

    VectorTileTree();
    explicit VectorTileTree(Options options);

    /// viewRect 允许跨反经线(west > east,自动拆两段枚举)。
    /// cameraHeightMeters 为相机离地高度。
    UpdateResult update(const Rectangle& viewRect, double cameraHeightMeters);

    /// 解码完成回灌。未请求过的 key 也接受(幂等)。
    void provide(const TileKey& key, MvtTile tile);
    /// 共享回灌(获取层单一化):瓦片由 MvtTileFetchCache 解码并共享持有,
    /// 树内只存引用 —— 同一块解码瓦在 drape/场/符号三消费方之间恰一份。
    /// 空指针视为失败(等价 markFailed)。
    void provideShared(const TileKey& key,
                       std::shared_ptr<const MvtTile> tile);
    /// 请求失败登记;失败瓦片不再重复请求,直到 clearFailed()。
    void markFailed(const TileKey& key);
    void clearFailed();

    const MvtTile* loadedTile(const TileKey& key) const;

    /// 共享持有已解码瓦片。E1:镶嵌在 worker 上跑,而树的 LRU 随时可能
    /// 淘汰该瓦片 —— worker 必须持共享所有权,不能拿裸指针。
    std::shared_ptr<const MvtTile> loadedTileShared(const TileKey& key) const;
    size_t loadedCount() const { return loaded_.size(); }
    size_t pendingCount() const { return pending_.size(); }
    size_t failedCount() const { return failed_.size(); }

    const TileScheme& scheme() const { return *scheme_; }
    const Options& options() const { return options_; }

    /// zoom ≈ log2(赤道周长 4e7m / 视高)(与 FeatureRenderLayer 的样式
    /// zoom 同一公式),加偏置后钳制 [minZoom, maxZoom]。
    static int zoomForCameraHeight(double cameraHeightMeters,
                                   const Options& options);

private:
    struct CachedTile {
        std::shared_ptr<const MvtTile> tile;
        uint64_t lastUsedFrame = 0;
    };

    void touch(const TileKey& key);
    void evictOverBudget();

    Options options_;
    std::unique_ptr<TileScheme> scheme_;
    SchemeId schemeId_;

    std::unordered_map<TileKey, CachedTile> loaded_;
    /// 每层已加载瓦计数(R* 后代回退的早退闸:更深层无存货就不下探)。
    /// 与 loaded_ 同步维护(insert/erase 两处),32 层覆盖 WebMercator 全域。
    std::array<int, 32> loadedPerZ_{};
    std::unordered_set<TileKey> pending_;
    std::unordered_set<TileKey> failed_;
    uint64_t frame_ = 0;
};

} // namespace earth_engine
