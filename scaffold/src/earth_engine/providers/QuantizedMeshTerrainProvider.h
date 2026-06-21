#pragma once

#include "TerrainProvider.h"
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace earth_engine {

class PlatformBridge;

/// Terrain provider for the quantized-mesh-1.0 format.
/// Parses binary tiles into first-class SurfaceTileMesh load results.
/// Synchronous heightmap decoding is intentionally unsupported for this
/// provider; quantized-mesh content enters the tile lifecycle through
/// requestTile, matching cesium-native's content-loader ownership model.
///
/// Reference: CesiumQuantizedMeshTerrain/QuantizedMeshLoader
class QuantizedMeshTerrainProvider : public TerrainProvider {
public:
    /// @param urlTemplate URL with {z}/{x}/{y} placeholders
    /// @param attribution display credit
    explicit QuantizedMeshTerrainProvider(std::string urlTemplate,
                                          std::string attribution = "");

    ~QuantizedMeshTerrainProvider() override;

    std::string id() const override;
    std::string type() const override { return "quantized-mesh-terrain"; }

    std::string schemeId() const override { return schemeId_; }

    int minZoom() const override { return minZoom_; }
    int maxZoom() const override { return maxZoom_; }
    int tileSize() const override { return tileSize_; }
    std::string attribution() const override { return attribution_; }

    void setZoomRange(int minZ, int maxZ);
    void setTileSize(int ts) { tileSize_ = ts; }
    void setPlatformBridge(PlatformBridge* bridge);
    void setFlipYForUrl(bool flip) { flipYForUrl_ = flip; }
    void setWaterMaskEnabled(bool enabled) { waterMaskEnabled_ = enabled; }
    bool configureFromLayerJsonUrl(const std::string& layerJsonUrl);
    bool configureFromLayerJson(const std::string& layerJson,
                                const std::string& layerJsonUrl);
    const std::string& urlTemplate() const { return urlTemplate_; }

    bool supportsTile(const TileKey& key) const override;
    TileAvailabilityState availabilityState(const TileKey& key) const override;
    std::string buildUrl(const TileKey& key) const override;
    int estimatedRequestFanout(const TileKey& key) const override;

    /// cesium-native: dynamically add availability from QM metadata
    void addAvailabilityRects(int level, const std::vector<TileAvailabilityRect>& rects);
    void addAvailabilityRectsForTile(
        const TileKey& subtreeKey,
        int level,
        const std::vector<TileAvailabilityRect>& rects);
    /// cesium-native: track loaded subtrees for sparse datasets
    bool isSubtreeLoaded(int subtreeLevel, uint64_t mortonIndex) const;
    void markSubtreeLoaded(int subtreeLevel, uint64_t mortonIndex);
    void markSubtreeLoadedForTile(const TileKey& subtreeKey);
    int availabilityLevels() const { return availabilityLevels_; }
    bool isAvailabilityBoundaryLevel(int level) const;
    void applyAvailabilityUpdates(
        const std::vector<QuantizedMeshAvailabilityUpdate>& updates);

    void requestTile(const TileKey& key,
                     CancellationToken token,
                     HeightmapCallback callback,
                     HttpRequestPriority priority =
                         HttpRequestPriority::Normal) override;

    ProviderRequestDiagnostics requestDiagnostics() const override;

    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t* data, size_t len) override;

private:
    struct LayerConfig {
        std::string urlTemplate;
        std::string layerJsonUrl;
        std::string schemeId = "Geographic-TMS";
        std::string version;
        std::string extensionsToRequest;
        std::vector<std::vector<TileAvailabilityRect>> availabilityRanges;
        std::vector<std::unordered_set<uint64_t>> loadedSubtrees;
        bool hasAvailability = false;
        int availabilityLevels = -1;
        int minZoom = 0;
        int maxZoom = 15;
        std::string attribution;
    };
    struct LayerAvailabilityRequest {
        size_t layerIndex = 0;
        TileKey subtreeKey;
        std::string url;
    };

    bool appendLayerFromJson(const nlohmann::json& j,
                             const std::string& layerJsonUrl,
                             const std::string& forcedSchemeId = {});
    bool appendParentLayers(const nlohmann::json& j,
                            const std::string& layerJsonUrl);
    size_t firstAvailableLayerIndex(const TileKey& key) const;
    const LayerConfig* firstAvailableLayer(const TileKey& key) const;
    LayerConfig* firstAvailableLayer(const TileKey& key);
    std::vector<LayerAvailabilityRequest>
    collectUnderlyingLayerAvailabilityRequests(const TileKey& key) const;
    TileAvailabilityState availabilityStateInLayer(
        const LayerConfig& layer,
        const TileKey& key) const;
    uint32_t maximumAvailableLevelAtTileCenter(
        const LayerConfig& layer,
        const TileKey& key) const;
    bool isSubtreeLoadedInLayer(
        const LayerConfig& layer,
        int subtreeLevel,
        uint64_t mortonIndex) const;
    void markSubtreeLoadedInLayer(
        LayerConfig& layer,
        int subtreeLevel,
        uint64_t mortonIndex);
    void addAvailabilityRectsToLayer(
        LayerConfig& layer,
        int level,
        const std::vector<TileAvailabilityRect>& rects);
    std::string buildUrlForLayer(const LayerConfig& layer,
                                 const TileKey& key) const;
    void syncLegacyFieldsFromPrimaryLayer();
    void handleAsyncTileBody(
        const TileKey& key,
        std::vector<LayerAvailabilityRequest> availabilityRequests,
        CancellationToken token,
        HeightmapCallback callback,
        HttpRequestPriority priority,
        int statusCode,
        std::vector<uint8_t> body,
        bool usePlatformBridge);
    void requestAsyncMetadataAndFinalize(
        TileKey key,
        std::shared_ptr<std::vector<LayerAvailabilityRequest>>
            availabilityRequests,
        std::shared_ptr<CancellationToken> token,
        std::shared_ptr<HeightmapCallback> callback,
        std::shared_ptr<std::vector<uint8_t>> body,
        int statusCode,
        HttpRequestPriority priority,
        bool usePlatformBridge);
    void finalizeAsyncTileRequest(
        TileKey key,
        std::shared_ptr<std::vector<LayerAvailabilityRequest>>
            availabilityRequests,
        std::shared_ptr<CancellationToken> token,
        std::shared_ptr<HeightmapCallback> callback,
        std::shared_ptr<std::vector<uint8_t>> body,
        int statusCode,
        std::vector<std::vector<uint8_t>> metadataBodies);
    std::vector<uint8_t> httpGet(
        const std::string& url,
        HttpRequestPriority priority = HttpRequestPriority::Normal,
        std::function<bool()> shouldCancel = {});
    std::vector<LayerConfig> layers_;
    std::string urlTemplate_;
    std::string attribution_;
    std::string layerJsonUrl_;
    std::string schemeId_ = "Geographic-TMS";
    std::string version_;
    std::string extensionsToRequest_;
    std::vector<std::vector<TileAvailabilityRect>> availabilityRanges_;
    std::vector<std::unordered_set<uint64_t>> loadedSubtrees_;
    bool hasAvailability_ = false;
    int availabilityLevels_ = -1;  // -1 = not using subtree mode
    int minZoom_ = 0;
    int maxZoom_ = 15;
    int tileSize_ = 65;   // default 64×64 grid
    bool flipYForUrl_ = false;
    bool waterMaskEnabled_ = false;
    PlatformBridge* platformBridge_ = nullptr;
    std::atomic<int> requestsStarted_{0};
    std::atomic<int> requestsCompleted_{0};
    std::atomic<int> activeWorkerBlockingRequests_{0};
    std::atomic<int> peakWorkerBlockingRequests_{0};
};

} // namespace earth_engine
