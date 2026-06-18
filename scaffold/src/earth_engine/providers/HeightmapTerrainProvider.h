#pragma once

#include "TerrainProvider.h"
#include <string>

namespace earth_engine {

class PlatformBridge;

/// 高度图地形 Provider。
///
/// 支持 Mapzen Terrarium 和 Mapbox Terrain-RGB 两种 RGB 高程编码。
///
/// Terrarium:
///   height(m) = R*256 + G + B/256 - 32768
///
/// Mapbox Terrain-RGB:
///   height(m) = -10000 + (R*256*256 + G*256 + B) * 0.1
///
/// URL 模板支持 {z}/{x}/{y} 占位符。
/// 示例：
///   https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png
///   file:///data/local/tmp/tiles/fabdem-raster-dem/{z}/{x}/{y}.png
class HeightmapTerrainProvider : public TerrainProvider {
public:
    enum class Encoding {
        Terrarium,
        MapboxTerrainRgb
    };

    /// @param urlTemplate URL 模板
    /// @param attribution 版权声明
    explicit HeightmapTerrainProvider(std::string urlTemplate,
                                       std::string attribution = "");
    ~HeightmapTerrainProvider() override;

    /// 注入平台 HTTP 桥接
    void setPlatformBridge(PlatformBridge* bridge);

    std::string id() const override;
    std::string type() const override { return "heightmap-terrain"; }
    std::string schemeId() const override { return "XYZ-WebMercator"; }

    int minZoom() const override { return minZoom_; }
    int maxZoom() const override { return maxZoom_; }
    int tileSize() const override { return tileSize_; }

    void setZoomRange(int minZ, int maxZ);
    void setMaxNativeZoom(int maxNativeZ) { maxNativeZoom_ = maxNativeZ; }
    int maxNativeZoom() const { return maxNativeZoom_; }
    void setEncoding(Encoding encoding);
    void setTileSize(int size) { tileSize_ = size; }
    void setHeightFactor(float factor) { heightFactor_ = factor; }
    void setNoDataValues(std::vector<float> values) { noDataValues_ = std::move(values); }

    std::string buildUrl(const TileKey& key) const override;

    void requestTile(const TileKey& key,
                     CancellationToken token,
                     HeightmapCallback callback,
                     HttpRequestPriority priority =
                         HttpRequestPriority::Normal) override;

    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t* data, size_t len) override;

private:
    std::vector<uint8_t> httpGet(const std::string& url,
                                 const CancellationToken& token,
                                 HttpRequestPriority priority);

    std::string urlTemplate_;
    std::string attribution_;
    Encoding encoding_ = Encoding::Terrarium;
    int minZoom_ = 0;
    int maxZoom_ = 14;
    int maxNativeZoom_ = 14;
    int tileSize_ = 256;
    float heightFactor_ = 1.0f;
    std::vector<float> noDataValues_;
    PlatformBridge* platformBridge_ = nullptr;
};

} // namespace earth_engine
