#pragma once

#include "TileKey.h"
#include "../core/math/Rectangle.h"
#include "../tiling/SurfaceTile.h"
#include "../providers/TerrainProvider.h"
#include "../renderer/RenderDevice.h"

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

    // ---- Raster overlays ----
    std::vector<std::unique_ptr<RasterMappedToTilesetTile>> rasterOverlays;

    // ---- LOD state ----
    bool renderable = false;
    bool subdivisionDesired = false;

    TilesetTile() = default;
    TilesetTile(TileKey k, Rectangle b) : key(std::move(k)), bounds(b) {}
};

} // namespace earth_engine
