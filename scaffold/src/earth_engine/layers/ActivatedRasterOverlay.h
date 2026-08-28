#pragma once

#include "../tiling/RasterOverlayProjection.h"
#include "../tiling/TileRasterOverlayUploadResult.h"

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

    TileRasterOverlayUploadResult processPendingUploads(
        bool interactionActive,
        FrameResourceBudget* budget = nullptr);
    bool hasPendingWork() const;
    uint64_t revision() const;
    void setFrameNumber(uint64_t frameNumber);
    void trimUnusedTiles(bool cachePressure = false);
    void invalidateDirectExecutionState();
    int64_t tileTextureBytesUsed() const;
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

    /// **生效**的采样投影(不是配置请求值)。georeference 会被 scheme 闸口拒绝,
    /// 请求 GCJ 而落成 merc/geo 时画面上与「没配」无法区分,所以诊断必须读这个。
    RasterOverlayProjection getProjection() const;

private:
    void syncProviderOptionsFromOverlay();

    RasterOverlay& overlay_;
    std::unique_ptr<RasterOverlayTileProvider> placeholderProvider_;
    std::unique_ptr<RasterOverlayTileProvider> tileProvider_;
    int maximumSimultaneousTileLoads_ = 20;
};

} // namespace earth_engine
