#pragma once

#include <cstdint>

namespace earth_engine {

class Tile;
class RasterOverlayTile;
struct TileKey;

/// cesium-native IPrepareRendererResources equivalent.
///
/// Abstract interface for attaching/detaching raster overlay textures
/// to/from geometry tiles. The implementation (typically Renderer)
/// manages the retained mapping from geometry tile → raster texture + UV.
///
/// This enables:
///  - Retained mode: attach once, render every frame without per-frame lookup
///  - Proper lifecycle: detach on eviction or tile replacement
///  - Testability: mock implementation for unit tests
class IPrepareRendererResources {
public:
    virtual ~IPrepareRendererResources() = default;

    /// cesium-native: attachRasterInMainThread.
    /// Registers a raster texture + UV transform for the given geometry tile.
    /// Called from RasterMappedToTilesetTile::update() Step 6.
    /// @param geometryKey   The geometry tile's quadtree key.
    /// @param overlayIndex  Which overlay slot (0-based). Used to key
    ///                      the attachment so multiple overlays can coexist.
    /// @param rasterTile    The raster overlay tile whose texture is being attached.
    /// @param texture       The GPU texture (owned by texture cache, non-null).
    /// @param translationU  Horizontal UV offset.
    /// @param translationV  Vertical UV offset.
    /// @param scaleU        Horizontal UV scale.
    /// @param scaleV        Vertical UV scale.
    virtual void attachRasterInMainThread(
        const TileKey& geometryKey,
        int32_t overlayIndex,
        const RasterOverlayTile& rasterTile,
        Texture* texture,
        float translationU, float translationV,
        float scaleU, float scaleV) = 0;

    /// cesium-native: detachRasterInMainThread.
    /// Removes the raster attachment for the given geometry tile + overlay slot.
    /// Called on eviction, tile replacement, or loading→ready promotion.
    /// Safe to call even if no attachment exists (no-op).
    /// @param geometryKey  The geometry tile's quadtree key.
    /// @param overlayIndex Which overlay slot to detach.
    virtual void detachRasterInMainThread(
        const TileKey& geometryKey,
        int32_t overlayIndex) noexcept = 0;
};

} // namespace earth_engine
