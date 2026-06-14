#pragma once

#include "TerrainProvider.h"
#include <array>
#include <string>
#include <unordered_set>

namespace earth_engine {

class PlatformBridge;

/// Terrain provider for the quantized-mesh-1.0 format.
/// Parses binary tiles and rasterizes them into DecodedHeightmap grids
/// for compatibility with the existing SurfaceTileMesh pipeline.
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

    std::string schemeId() const override { return "Geographic-TMS"; }

    int minZoom() const override { return minZoom_; }
    int maxZoom() const override { return maxZoom_; }
    int tileSize() const override { return tileSize_; }

    void setZoomRange(int minZ, int maxZ);
    void setTileSize(int ts) { tileSize_ = ts; }
    void setPlatformBridge(PlatformBridge* bridge);
    void setFlipYForUrl(bool flip) { flipYForUrl_ = flip; }
    bool configureFromLayerJsonUrl(const std::string& layerJsonUrl);
    bool configureFromLayerJson(const std::string& layerJson,
                                const std::string& layerJsonUrl);
    const std::string& urlTemplate() const { return urlTemplate_; }

    bool supportsTile(const TileKey& key) const override;
    std::string buildUrl(const TileKey& key) const override;

    /// cesium-native: dynamically add availability from QM metadata
    void addAvailabilityRects(int level, const std::vector<std::array<int, 4>>& rects);
    /// cesium-native: track loaded subtrees for sparse datasets
    bool isSubtreeLoaded(int subtreeLevel, uint64_t mortonIndex) const;
    void markSubtreeLoaded(int subtreeLevel, uint64_t mortonIndex);
    int availabilityLevels() const { return availabilityLevels_; }

    void requestTile(const TileKey& key,
                     CancellationToken token,
                     HeightmapCallback callback) override;

    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t* data, size_t len) override;

private:
    std::vector<uint8_t> httpGet(const std::string& url);
    std::string urlTemplate_;
    std::string attribution_;
    std::string layerJsonUrl_;
    std::vector<std::vector<std::array<int, 4>>> availabilityRanges_;
    std::vector<std::unordered_set<uint64_t>> loadedSubtrees_;
    int availabilityLevels_ = -1;  // -1 = not using subtree mode
    int minZoom_ = 0;
    int maxZoom_ = 15;
    int tileSize_ = 65;   // default 64×64 grid
    bool flipYForUrl_ = false;
    PlatformBridge* platformBridge_ = nullptr;
};

} // namespace earth_engine
