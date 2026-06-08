#pragma once

#include "TileKey.h"
#include "../core/math/Rectangle.h"
#include "../core/math/Vec3.h"
#include "../renderer/RenderDevice.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace earth_engine {

enum class SurfaceProfile {
    Ellipsoid,
    Terrain
};

enum class ImageryFallbackSource {
    Exact,
    Parent,
    Placeholder
};

enum class SurfaceTileMeshWinding {
    Outward
};

enum class SurfaceTileSampling {
    WebMercatorVToWgs84Ecef,
    GeographicVToWgs84Ecef
};

struct SurfaceVertex {
    Vec3 positionEcef;
    Vec3 normalEcef;
    std::array<float, 2> uv = {0.0f, 0.0f};
};

struct SurfaceTileMesh {
    std::vector<SurfaceVertex> vertices;
    std::vector<uint32_t> indices;
    int gridSize = 0;
    SurfaceTileMeshWinding winding = SurfaceTileMeshWinding::Outward;
    SurfaceTileSampling sampling = SurfaceTileSampling::WebMercatorVToWgs84Ecef;
};

struct ImageryAttachment {
    std::string layerId;
    std::string providerId;
    TileKey textureKey;
    Texture* texture = nullptr;
    float uvOffsetU = 0.0f;
    float uvOffsetV = 0.0f;
    float uvScaleU = 1.0f;
    float uvScaleV = 1.0f;
    float opacity = 1.0f;
    ImageryFallbackSource fallbackSource = ImageryFallbackSource::Exact;
};

struct SurfaceTileKey {
    TileKey tileKey;
    SurfaceProfile profile = SurfaceProfile::Ellipsoid;
    std::string terrainVersion;
};

struct SurfaceTile {
    SurfaceTileKey key;
    Rectangle bounds;
    SurfaceTileMesh mesh;
    std::vector<ImageryAttachment> imageryAttachments;
    uint64_t generation = 0;
};

} // namespace earth_engine
