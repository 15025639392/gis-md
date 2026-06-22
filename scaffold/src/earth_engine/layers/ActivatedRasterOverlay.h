#pragma once

#include <cstdint>
#include <memory>

namespace earth_engine {

class RasterOverlay;
class RasterOverlayTile;
class RasterOverlayTileProvider;
class FrameResourceBudget;
class RenderDevice;

/// cesium-native ActivatedRasterOverlay equivalent.
///
/// Wraps a RasterOverlay with runtime state: owns the tile provider,
/// manages throttling, and provides the placeholder tile.
///
/// This separates the "activated" runtime layer from the pure-config
/// RasterOverlay and the Tileset renderer.
class ActivatedRasterOverlay {
public:
    /// @param overlay The raster overlay configuration (must outlive this).
    explicit ActivatedRasterOverlay(RasterOverlay& overlay);
    ~ActivatedRasterOverlay();

    ActivatedRasterOverlay(const ActivatedRasterOverlay&) = delete;
    ActivatedRasterOverlay& operator=(const ActivatedRasterOverlay&) = delete;

    /// Ensure the runtime provider exists. This mirrors cesium-native
    /// activation, where an overlay has runtime state before its tiles are
    /// mapped to already-loaded geometry.
    RasterOverlayTileProvider* ensureTileProvider(RenderDevice* device);

    /// cesium-native: the active tile provider (null until ensureTileProvider).
    RasterOverlayTileProvider* getTileProvider() { return tileProvider_.get(); }
    const RasterOverlayTileProvider* getTileProvider() const { return tileProvider_.get(); }

    /// cesium-native: the placeholder tile from the ensured provider.
    RasterOverlayTile* getPlaceholderTile();

    int processPendingUploads(bool interactionActive,
                              FrameResourceBudget* budget = nullptr);
    bool hasPendingWork() const;
    uint64_t revision() const;
    void setFrameNumber(uint64_t frameNumber);
    void trimUnusedTiles();
    int getCachedTileCount() const;

    /// Maximum simultaneous tile loads (aligned with RasterOverlayOptions).
    int getMaximumSimultaneousTileLoads() const { return maximumSimultaneousTileLoads_; }
    void setMaximumSimultaneousTileLoads(int n);
    int getThrottledTilesCurrentlyLoading() const;

    bool visible() const;
    float opacity() const;

    // ── Accessors ──

    /// The overlay configuration.
    RasterOverlay& getOverlay() { return overlay_; }
    const RasterOverlay& getOverlay() const { return overlay_; }

private:
    void syncProviderOptionsFromOverlay();

    RasterOverlay& overlay_;
    std::unique_ptr<RasterOverlayTileProvider> placeholderProvider_;
    std::unique_ptr<RasterOverlayTileProvider> tileProvider_;
    int maximumSimultaneousTileLoads_ = 20;
};

} // namespace earth_engine
