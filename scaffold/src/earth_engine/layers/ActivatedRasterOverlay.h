#pragma once

#include "../providers/RasterOverlayTile.h"

#include <memory>

namespace earth_engine {

class RasterOverlay;
class RasterOverlayTileProvider;

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

    // ── Provider management ──

    /// cesium-native: set the real tile provider (resolves ready event).
    void setTileProvider(std::unique_ptr<RasterOverlayTileProvider> provider);

    /// cesium-native: the active tile provider (null before setTileProvider).
    RasterOverlayTileProvider* getTileProvider() { return tileProvider_.get(); }
    const RasterOverlayTileProvider* getTileProvider() const { return tileProvider_.get(); }

    /// cesium-native: the placeholder tile (valid even before provider is ready).
    RasterOverlayTile* getPlaceholderTile();

    // ── Tile loading with throttling ──

    /// cesium-native: get a tile (delegates to provider, or placeholder).
    RasterOverlayTile* getTile(const TileKey& key);

    /// cesium-native: throttled tile load.
    bool loadTileThrottled(RasterOverlayTile& tile);

    void processPendingUploads();
    void setFrameNumber(uint64_t frameNumber);
    void trimUnusedTiles();
    int getCachedTileCount() const;

    /// Maximum simultaneous tile loads (aligned with RasterOverlayOptions).
    int getMaximumSimultaneousTileLoads() const { return maximumSimultaneousTileLoads_; }
    void setMaximumSimultaneousTileLoads(int n) { maximumSimultaneousTileLoads_ = n; }
    int getThrottledTilesCurrentlyLoading() const;

    bool visible() const;
    float opacity() const;

    // ── Accessors ──

    /// The overlay configuration.
    RasterOverlay& getOverlay() { return overlay_; }
    const RasterOverlay& getOverlay() const { return overlay_; }

private:
    RasterOverlay& overlay_;
    std::unique_ptr<RasterOverlayTileProvider> tileProvider_;
    std::unique_ptr<RasterOverlayTile> placeholderTile_;
    int maximumSimultaneousTileLoads_ = 20;
};

} // namespace earth_engine
