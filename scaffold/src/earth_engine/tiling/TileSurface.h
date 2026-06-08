#pragma once

#include "SurfaceTile.h"
#include "TileKey.h"
#include "../core/math/Rectangle.h"
#include "../core/math/Vec3.h"

#include <array>

namespace earth_engine {

class TileScheme;

/// CPU-side contract for draping a raster tile on the WGS84 ellipsoid.
/// Coordinates are radians, ECEF is meters, UV origin is texture top-left.
struct TileSurfaceVertex {
    Vec3 ecef;
    std::array<float, 2> uv;
};

struct TileTextureWindow {
    float offsetU = 0.0f;
    float offsetV = 0.0f;
    float scaleU = 1.0f;
    float scaleV = 1.0f;
};

class TileSurface {
public:
    static TileSurfaceVertex vertexForUnitUv(const Rectangle& tileBounds,
                                             double u,
                                             double v);

    static TileTextureWindow textureWindow(const Rectangle& targetBounds,
                                           const Rectangle& textureBounds);

    static SurfaceTileMesh buildEllipsoidMesh(const Rectangle& tileBounds,
                                              int gridSize);

    static bool trianglesFaceOutward(const Rectangle& tileBounds);

    static Rectangle boundsForKey(const TileScheme& scheme, const TileKey& key);
};

} // namespace earth_engine
