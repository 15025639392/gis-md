#pragma once

#include "../tiling/TileKey.h"
#include "../platform/bridge/PlatformBridge.h"
#include "../threading/CancellationToken.h"

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace earth_engine {

/// 解码后的高度图数据。
/// 高度为 WGS84 ellipsoid height（meter）。
/// 网格为 tileSize×tileSize 的 regular grid。
struct DecodedHeightmap {
    int tileSize = 0;                  // 每边采样数（如 256）
    std::vector<float> heights;        // tileSize × tileSize，行优先（北→南）
    float minHeight = 0.0f;
    float maxHeight = 0.0f;

    /// 无效高度哨兵值列表（OpenGlobus noDataValues）
    std::vector<float> noDataValues;

    /// 高度缩放因子（OpenGlobus _heightFactor），默认为 1.0
    float heightFactor = 1.0f;

    bool valid() const { return tileSize > 0 && heights.size() == static_cast<size_t>(tileSize * tileSize); }

    /// 检查高度是否为无效值（哨兵值匹配 或 值 > 50000）
    bool isNoData(float height) const;

    /// 双线性采样高度。
    /// @param u 列归一化坐标 [0,1]（西→东）
    /// @param v 行归一化坐标 [0,1]（北→南）
    float sampleBilinear(float u, float v) const;

    /// Raw binary data from the provider (e.g. QuantizedMesh bytes).
    /// When non-empty, TileSurface::buildTerrainMesh may reconstruct
    /// the optimized triangulation instead of using the regular grid.
    std::vector<uint8_t> rawData;

    /// cesium-native: availability rectangles from QM metadata (extension ID=4)
    std::vector<std::array<int, 4>> metadataAvailability;
};

/// 地形数据 Provider 抽象接口。
/// 负责 URL 构建、网络请求和高度图解碼。不持有 GPU 资源。
class TerrainProvider {
public:
    virtual ~TerrainProvider() = default;

    /// 唯一标识
    virtual std::string id() const = 0;

    /// Provider 类型
    virtual std::string type() const { return "terrain"; }

    /// 瓦片体系 ID
    virtual std::string schemeId() const = 0;

    /// 最小/最大 zoom
    virtual int minZoom() const = 0;
    virtual int maxZoom() const = 0;

    /// 瓦片尺寸（像素）
    virtual int tileSize() const = 0;

    /// Whether this provider can serve the tile. Providers with metadata
    /// availability should reject unsupported tiles before network request.
    virtual bool supportsTile(const TileKey& key) const {
        (void)key;
        return true;
    }

    /// 构建瓦片请求 URL
    virtual std::string buildUrl(const TileKey& key) const = 0;

    using HeightmapCallback = std::function<void(
        const TileKey&, std::unique_ptr<DecodedHeightmap>)>;

    /// 异步请求高度图瓦片
    virtual void requestTile(const TileKey& key,
                             CancellationToken token,
                             HeightmapCallback callback) = 0;

    /// 同步解码高度图（用于测试/调试）
    virtual std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t* data, size_t len) = 0;
};

} // namespace earth_engine
