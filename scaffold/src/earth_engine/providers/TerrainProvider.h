#pragma once

#include "ProviderRequestDiagnostics.h"
#include "../content/GltfModel.h"
#include "../tiling/TileKey.h"
#include "../tiling/TileBoundingVolume.h"
#include "../tiling/TileLoadResultMetadata.h"
#include "../tiling/TileLoadStatus.h"
#include "../tiling/SurfaceTile.h"
#include "../platform/bridge/PlatformBridge.h"
#include "../threading/CancellationToken.h"

#include <optional>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <array>
#include <utility>

namespace earth_engine {

enum class TileAvailabilityState {
    NotAvailable,
    Available,
    Unknown
};

/// cesium-native LayerJsonTerrainLoader: when loading a tile from an
/// upper layer, availability metadata for matching underlying layers is
/// loaded at the same time. These updates are applied on the main thread.
struct QuantizedMeshAvailabilityUpdate {
    int layerIndex = -1;
    TileKey subtreeKey;
    std::vector<QuantizedMeshAvailabilityRange> metadataAvailability;
};

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

    /// cesium-native: availability rectangles from QM metadata (extension ID=4).
    /// Each entry: {levelOffset, startX, startY, endX, endY}
    std::vector<QuantizedMeshAvailabilityRange> metadataAvailability;
    bool metadataAvailabilityProcessed = false;
};

enum class TerrainTilePayloadKind {
    None,
    Heightmap,
    GltfModel
};

struct TerrainTileLoadResult {
    TileLoadStatus status = TileLoadStatus::Failed;
    TerrainTilePayloadKind payloadKind = TerrainTilePayloadKind::None;
    std::unique_ptr<DecodedHeightmap> heightmap;
    std::unique_ptr<GltfModel> gltfModel;
    TileLoadResultMetadata metadata;
    std::vector<QuantizedMeshAvailabilityUpdate>
        quantizedMeshAvailabilityUpdates;

    static TerrainTileLoadResult successWithHeightmap(
        std::unique_ptr<DecodedHeightmap> hm) {
        TerrainTileLoadResult result;
        result.status = hm ? TileLoadStatus::Renderable
                           : TileLoadStatus::Failed;
        result.payloadKind = hm ? TerrainTilePayloadKind::Heightmap
                                : TerrainTilePayloadKind::None;
        result.heightmap = std::move(hm);
        return result;
    }

    static TerrainTileLoadResult successWithGltfModel(
        std::unique_ptr<GltfModel> model,
        TileLoadResultMetadata metadata = {}) {
        TerrainTileLoadResult result;
        result.status = model ? TileLoadStatus::Renderable
                              : TileLoadStatus::Failed;
        result.payloadKind = model ? TerrainTilePayloadKind::GltfModel
                                   : TerrainTilePayloadKind::None;
        result.metadata = std::move(metadata);
        if (model && !result.metadata.rasterOverlayDetails.has_value()) {
            result.metadata.rasterOverlayDetails =
                model->rasterOverlayDetails;
        }
        result.gltfModel = std::move(model);
        return result;
    }

    static TerrainTileLoadResult empty() {
        TerrainTileLoadResult result;
        result.status = TileLoadStatus::Empty;
        return result;
    }

    static TerrainTileLoadResult retryLater() {
        TerrainTileLoadResult result;
        result.status = TileLoadStatus::RetryLater;
        return result;
    }

    static TerrainTileLoadResult failed() {
        TerrainTileLoadResult result;
        result.status = TileLoadStatus::Failed;
        return result;
    }

    static TerrainTileLoadResult cancelled() {
        TerrainTileLoadResult result;
        result.status = TileLoadStatus::Cancelled;
        return result;
    }
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

    /// Display credit / attribution for this terrain source.
    virtual std::string attribution() const { return ""; }

    /// Whether this provider can serve the tile. Providers with metadata
    /// availability should reject unsupported tiles before network request.
    virtual bool supportsTile(const TileKey& key) const {
        return availabilityState(key) == TileAvailabilityState::Available;
    }

    /// cesium-native LayerJsonTerrainLoader::tileIsAvailableInLayer equivalent.
    /// Available means the content can be requested now. Unknown means a
    /// metadata availability subtree has not been loaded yet.
    virtual TileAvailabilityState availabilityState(const TileKey& key) const {
        if (key.schemeId != schemeId() ||
            key.z < minZoom() ||
            key.z > maxZoom()) {
            return TileAvailabilityState::NotAvailable;
        }
        return TileAvailabilityState::Available;
    }

    /// 构建瓦片请求 URL
    virtual std::string buildUrl(const TileKey& key) const = 0;

    /// Estimated transport requests issued by requestTile for this logical
    /// terrain tile. Providers that fan out metadata/subtree fetches should
    /// include those requests so frame budgets reflect real network pressure.
    virtual int estimatedRequestFanout(const TileKey&) const { return 1; }

    /// cesium-native LayerJsonTerrainLoader availability subtree boundary.
    /// Traversal waits for boundary tile content before materializing unknown
    /// children so sparse terrain does not issue blind descendant requests.
    virtual bool isAvailabilityBoundaryLevel(int) const { return false; }

    /// Apply availability discovered while loading terrain content. Quantized
    /// mesh providers use this for metadata extension updates; providers that
    /// do not expose sparse availability ignore it.
    virtual void applyAvailabilityUpdates(
        const std::vector<QuantizedMeshAvailabilityUpdate>&) {}

    using TerrainCallback = std::function<void(
        const TileKey&, TerrainTileLoadResult)>;

    /// Asynchronously request terrain tile content.
    virtual void requestTile(const TileKey& key,
                             CancellationToken token,
                             TerrainCallback callback,
                             HttpRequestPriority priority =
                                 HttpRequestPriority::Normal) = 0;

    virtual ProviderRequestDiagnostics requestDiagnostics() const {
        return {};
    }

    /// 同步解码高度图（用于测试/调试）
    virtual std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t* data, size_t len) = 0;
};

} // namespace earth_engine
