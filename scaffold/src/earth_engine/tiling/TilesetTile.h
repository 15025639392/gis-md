#pragma once

#include "TileKey.h"
#include "../core/math/Rectangle.h"
#include "../tiling/SurfaceTile.h"
#include "../providers/TerrainProvider.h"
#include "../renderer/RenderDevice.h"
#include "TileScheme.h"

#include <memory>
#include <vector>
#include <cstdint>

namespace earth_engine {

class RasterMappedToTilesetTile;

/// cesium-native Tile equivalent.
/// Each tile in the unified quadtree holds geometry (QM mesh) and
/// raster overlays (imagery). The tree uses GeographicTMSScheme.
struct TilesetTile {
    TileKey key;
    Rectangle bounds;          // geographic radians
    double geometricError = 0.0;

    // ---- Tree structure (cesium-native parent/child) ----
    TilesetTile* parent = nullptr;
    std::vector<TilesetTile*> children;  // raw pointers, owned by Tileset::tiles_ map

    // ---- Content (geometry) ----
    /// QM raw binary data (lazily decoded to SurfaceTileMesh)
    std::unique_ptr<DecodedHeightmap> heightmap;
    /// Parsed QM mesh (null until decoded)
    std::unique_ptr<SurfaceTileMesh> mesh;
    /// GPU vertex buffer for this tile's geometry
    std::unique_ptr<Buffer> gpuVertexBuffer;
    /// GPU index buffer (per-tile, from QM triangulation)
    std::unique_ptr<Buffer> gpuIndexBuffer;
    /// Tile-local origin (for RTC: positions are ECEF - origin)
    Vec3 localOrigin = Vec3::zero();
    /// Whether the mesh is ready for rendering
    bool meshReady = false;
    /// Last frame this tile was used (for LRU eviction)
    uint64_t lastUsedFrame = 0;

    // ---- Raster overlays ----
    std::vector<std::unique_ptr<RasterMappedToTilesetTile>> rasterOverlays;

    // ---- LOD state ----
    bool renderable = false;
    bool subdivisionDesired = false;

    TilesetTile() = default;
    TilesetTile(TileKey k, Rectangle b, TilesetTile* p = nullptr)
        : key(std::move(k)), bounds(b), parent(p) {}

    /// cesium-native: create 4 child tiles with inherited geometric error
    void createChildren(const TileScheme& scheme) {
        if (!children.empty() || key.z >= 22) return;
        int cz = key.z + 1;
        int cx = key.x * 2, cy = key.y * 2;
        double childError = geometricError * 0.5;
        for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
                TileKey childKey{key.schemeId, cz, cx + dx, cy + dy};
                auto child = std::make_unique<TilesetTile>(
                    childKey, scheme.tileToRectangle(childKey), this);
                child->geometricError = childError;
                children.push_back(std::move(child));
            }
        }
    }

    /// Find the deepest ancestor with a ready mesh (for upsampling)
    const TilesetTile* findUpsampleAncestor() const {
        const TilesetTile* p = parent;
        while (p) {
            if (p->meshReady && p->gpuVertexBuffer) return p;
            p = p->parent;
        }
        return nullptr;
    }
};

} // namespace earth_engine
