#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ImageryProvider.h"
#include "../data/AmapVectorSource.h"
#include "../data/MvtTileFetchCache.h"
#include "../data/VectorTileRasterizer.h"

namespace earth_engine {

class ThreadPool;

/// 高德 type2 面 drape 源:已解码的 Polygon Feature 动态栅格化冒充影像,
/// 进 TerrainPageStore 页合成(V1 面表示,与 MVT `VectorDrapeImageryProvider`
/// 同一出口)。
///
/// **有页时走本通路(V1)**:寄生地形合成,GPU ~0。无页(当前 demo 关地形)
/// 时本 overlay 拉不到瓦,type2 改由 `AmapRegionsVectorSource` → VectorFill
/// (V30 地球网格)。两条路共享 RegionCache,不要在主源再解一遍 type2。
///
/// **数据侧保留高德/xinzhi-map 的 2D 处理**(解码期裁剪 + even-odd 归一化),
/// **上屏走球上页合成**:页存储按屏幕清晰度要多深就现画多深(overzoom 子矩形),
/// 源瓦钳在 dataMaxZoom(区域档 z10)。
///
/// 页网格是 Web Mercator(与影像同轨);源瓦是 Amap 4326 等距圆柱。requestTile
/// 把页矩形转成 lon/lat,再 `amapCoverageFromUnitRect` 取覆盖的 4326 瓦。
/// 页是 WGS、Amap 网格是 GCJ 时,选瓦前 `shiftRectWgs84ToGcj`。
///
/// 失败语义同 VectorDrape:回全透明图而非 nullptr,避免页合成缓冲滞留。
/// 覆盖瓦数超过 maxSourceTiles 也回透明图(远景 z10 覆盖爆炸的地板)。
class AmapDrapeImageryProvider : public ImageryProvider {
public:
    using RegionCache =
        MvtTileFetchCacheT<std::vector<Feature>, AmapDecodeTraits<true>>;

    struct Options {
        std::string id = "amap-drape";
        std::string schemeId = "XYZ-WebMercator";
        int minZoom = 0;
        int advertisedMaxZoom = 18;
        int dataMinZoom = 10;
        int dataMaxZoom = 10;
        int tileSize = 256;
        int maxSourceTiles = 16;
        VectorRasterStyle style;
        bool gcj02SourceGrid = false;
    };

    AmapDrapeImageryProvider(Options options,
                             std::shared_ptr<RegionCache> tileCache,
                             std::shared_ptr<ThreadPool> rasterPool = nullptr);

    std::string id() const override { return options_.id; }
    std::string type() const override { return "amap-drape"; }
    std::string schemeId() const override { return options_.schemeId; }
    int minZoom() const override { return options_.minZoom; }
    int maxZoom() const override { return options_.advertisedMaxZoom; }
    int tileWidth() const override { return options_.tileSize; }
    int tileHeight() const override { return options_.tileSize; }

    std::string buildUrl(const TileKey& key) const override;

    void requestTile(const TileKey& key, CancellationToken token,
                     TileCallback callback,
                     HttpRequestPriority priority =
                         HttpRequestPriority::Normal) override;

    std::unique_ptr<DecodedImage> decodeTile(const uint8_t* data,
                                             size_t len) override;

    void setStyle(VectorRasterStyle style);

private:
    struct Assembly;

    static void runAssembly(const std::shared_ptr<Assembly>& assembly);
    static void completeIfReady(const std::shared_ptr<Assembly>& assembly);
    VectorRasterStyle styleSnapshot() const;

    Options options_;
    mutable std::mutex styleMutex_;
    std::shared_ptr<RegionCache> tileCache_;
    std::shared_ptr<ThreadPool> rasterPool_;
};

}  // namespace earth_engine
