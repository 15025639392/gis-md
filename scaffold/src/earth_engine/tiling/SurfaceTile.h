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
    Vec3 positionHighEcef;
    Vec3 positionLowEcef;
    Vec3 normalEcef;
    std::array<float, 2> uv = {0.0f, 0.0f};
};

/// cesium-native style skirt metadata (see CesiumGltfContent/SkirtMeshMetadata.h).
/// Tracks the vertex/index ranges that belong to the real terrain surface
/// (not the skirt), so downstream operations (e.g. normal map, UV generation)
/// can skip skirt geometry.
struct SkirtMetadata {
    uint32_t noSkirtIndicesBegin = 0;
    uint32_t noSkirtIndicesCount = 0;
    uint32_t noSkirtVerticesBegin = 0;
    uint32_t noSkirtVerticesCount = 0;
};

/// Water mask from QuantizedMesh extension ID=2.
/// If both allLand and allWater are false, data is a 256×256 RGBA8 bitmap.
struct WaterMask {
    std::vector<uint8_t> data;  // 256×256 RGBA8, empty = no mask
    bool allLand = true;
    bool allWater = false;
    bool valid() const { return !data.empty() || allWater; }
};

struct SurfaceTileMesh {
    std::vector<SurfaceVertex> vertices;
    std::vector<uint32_t> indices;
    int gridSize = 0;
    SurfaceTileMeshWinding winding = SurfaceTileMeshWinding::Outward;
    SurfaceTileSampling sampling = SurfaceTileSampling::WebMercatorVToWgs84Ecef;
    SkirtMetadata skirtMeta;
    WaterMask waterMask;
};

struct SurfaceNormalMap {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;

    bool valid() const {
        return width > 0 &&
               height > 0 &&
               rgba.size() == static_cast<size_t>(width * height * 4);
    }
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
