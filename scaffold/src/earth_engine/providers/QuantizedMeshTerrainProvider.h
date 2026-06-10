#pragma once

#include "TerrainProvider.h"
#include <string>

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

    std::string buildUrl(const TileKey& key) const override;

    void requestTile(const TileKey& key,
                     CancellationToken token,
                     HeightmapCallback callback) override;

    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t* data, size_t len) override;

private:
    std::vector<uint8_t> httpGet(const std::string& url);
    std::string urlTemplate_;
    std::string attribution_;
    int minZoom_ = 0;
    int maxZoom_ = 15;
    int tileSize_ = 65;   // default 64×64 grid
    bool flipYForUrl_ = false;
    PlatformBridge* platformBridge_ = nullptr;
};

} // namespace earth_engine
