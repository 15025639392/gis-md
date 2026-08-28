#pragma once

#include <cstdint>
#include <memory>

namespace earth_engine {

class Tile;
class RasterOverlayTile;
class Texture;
struct TileKey;

/// cesium-native IPrepareRendererResources equivalent.
///
/// Abstract resource lifecycle notification for raster overlay textures.
/// It must not be used as the source of truth for surface visibility,
/// ancestor fallback, or drawable raster binding decisions.
///
/// This enables:
///  - Resource hooks aligned with cesium-native attach/detach timing
///  - Proper lifecycle notification on eviction or tile replacement
///  - Testability with mock implementations for lifecycle events
class IPrepareRendererResources {
public:
    virtual ~IPrepareRendererResources() = default;

    /// cesium-native: attachRasterInMainThread.
    /// Notifies that a raster texture + UV transform is ready for the given
    /// geometry tile.
    /// Called from DirectRasterMapping::update() Step 6.
    /// @param geometryKey   The geometry tile's quadtree key.
    /// @param overlayIndex  Which overlay slot (0-based), so multiple overlay
    ///                      resource notifications can coexist.
    /// @param rasterTile    Raster overlay tile whose texture is ready.
    /// @param texture       The GPU texture (owned by texture cache, non-null).
    /// @param translationU  Horizontal UV offset.
    /// @param translationV  Vertical UV offset.
    /// @param scaleU        Horizontal UV scale.
    /// @param scaleV        Vertical UV scale.
    virtual void attachRasterInMainThread(
        const TileKey& geometryKey,
        int32_t overlayIndex,
        std::shared_ptr<const RasterOverlayTile> rasterTile,
        Texture* texture,
        float translationU, float translationV,
        float scaleU, float scaleV) = 0;

    /// cesium-native: detachRasterInMainThread.
    /// Notifies that raster resources for the geometry tile + overlay slot are
    /// no longer active.
    /// Called on eviction, tile replacement, or loading→ready promotion.
    /// Safe to call even if no resources are currently active (no-op).
    /// @param geometryKey  The geometry tile's quadtree key.
    /// @param overlayIndex Which overlay slot to detach.
    virtual void detachRasterInMainThread(
        const TileKey& geometryKey,
        int32_t overlayIndex) noexcept = 0;

    /// 北极星 Phase 2c P5b:共享位移模板几何是否活跃(= Renderer 持有
    /// TerrainDisplacementTemplatePool)。prepare 侧用它决定是否跳过「必走共享
    /// 模板」的 fine 地形瓦片的废弃 per-tile VBO 建/传(判据其余部分见
    /// GltfRenderResourcePreparer)。默认 false = 行为与 P5b 前逐字节一致,mock
    /// 实现零改动。
    virtual bool terrainSharedTemplateActive() const { return false; }
};

} // namespace earth_engine
