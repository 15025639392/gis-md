#pragma once

#include "../core/math/Rectangle.h"
#include "../core/math/Vec3.h"

#include <array>
#include <cstdint>
#include <vector>

namespace earth_engine {

using TileAvailabilityRect = std::array<uint32_t, 4>;
using QuantizedMeshAvailabilityRange = std::array<uint32_t, 5>;

enum class SurfaceTileMeshWinding {
    Outward
};

enum class SurfaceTileSampling {
    WebMercatorVToWgs84Ecef,
    GeographicVToWgs84Ecef
};

enum class RasterOverlayProjection {
    Geographic = 0
};

struct SurfaceVertex {
    Vec3 positionEcef;
    Vec3 positionHighEcef;
    Vec3 positionLowEcef;
    Vec3 normalEcef;
    std::array<float, 2> uv = {0.0f, 0.0f};
};

struct SurfaceGpuVertex {
    float pos[3];
    float nrm[3];
    float uv[2];
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
    double translationX = 0.0;
    double translationY = 0.0;
    double scale = 1.0;
    bool valid() const { return !data.empty() || allWater; }
};

/// cesium-native RasterOverlayDetails equivalent.
/// Stores the set of projections and their corresponding texture coordinate
/// rectangles for a geometry tile. The current rectangle overlay provider
/// exposes Geographic projection details.
struct RasterOverlayDetails {
    /// Projections for which texture coordinates exist.
    std::vector<RasterOverlayProjection> rasterOverlayProjections;

    /// Texture coordinate rectangles for each projection.
    /// Index-aligned with rasterOverlayProjections.
    std::vector<Rectangle> rasterOverlayRectangles;

    bool empty() const { return rasterOverlayRectangles.empty(); }

    const Rectangle* findRectangleForOverlayProjection(
        RasterOverlayProjection projection) const {
        for (size_t i = 0; i < rasterOverlayProjections.size(); ++i) {
            if (rasterOverlayProjections[i] == projection &&
                i < rasterOverlayRectangles.size()) {
                return &rasterOverlayRectangles[i];
            }
        }
        return nullptr;
    }

    int32_t textureCoordinateIDForProjection(
        RasterOverlayProjection projection) const {
        for (size_t i = 0; i < rasterOverlayProjections.size(); ++i) {
            if (rasterOverlayProjections[i] == projection &&
                i < rasterOverlayRectangles.size()) {
                return static_cast<int32_t>(i);
            }
        }
        return -1;
    }

    void setGeographicRectangle(const Rectangle& rectangle) {
        rasterOverlayProjections = {RasterOverlayProjection::Geographic};
        rasterOverlayRectangles = {rectangle};
    }

    void merge(const RasterOverlayDetails& other) {
        rasterOverlayProjections.insert(
            rasterOverlayProjections.end(),
            other.rasterOverlayProjections.begin(),
            other.rasterOverlayProjections.end());
        rasterOverlayRectangles.insert(
            rasterOverlayRectangles.end(),
            other.rasterOverlayRectangles.begin(),
            other.rasterOverlayRectangles.end());
    }
};

struct SurfaceTileMesh {
    std::vector<SurfaceVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<SurfaceGpuVertex> gpuVertices;
    int gridSize = 0;
    SurfaceTileMeshWinding winding = SurfaceTileMeshWinding::Outward;
    SurfaceTileSampling sampling = SurfaceTileSampling::WebMercatorVToWgs84Ecef;
    /// cesium-native quantized-mesh meshCenter / RTC origin.
    /// Geometry vertices remain absolute ECEF in this project; upload code
    /// subtracts this origin to produce small GPU coordinates.
    bool hasLocalOriginEcef = false;
    Vec3 localOriginEcef = Vec3::zero();
    /// cesium-native QuantizedMeshLoadResult::updatedBoundingVolume height range.
    /// These are the QuantizedMesh header minimum/maximum heights, not
    /// necessarily the min/max of the simplified vertex set.
    bool hasHeightRange = false;
    double minimumHeight = 0.0;
    double maximumHeight = 0.0;
    /// Quantized-mesh header horizon occlusion point, expressed in
    /// ellipsoid-scaled ECEF like cesium-native.
    bool hasHorizonOcclusionPoint = false;
    Vec3 horizonOcclusionPoint = Vec3::zero();
    SkirtMetadata skirtMeta;
    WaterMask waterMask;
    // cesium-native: availability rectangles from QM metadata (extension ID=4).
    // Each entry: {levelOffset, startX, startY, endX, endY}
    // levelOffset = sub-array index in the "available" JSON.
    // Actual absolute level = tileLevel + levelOffset
    // Aligned with cesium-native loadAvailabilityRectangles startingLevel + i.
    bool hasMetadataAvailability = false;
    std::vector<QuantizedMeshAvailabilityRange> metadataAvailability;
    /// cesium-native TileRenderContent::getRasterOverlayDetails equivalent.
    RasterOverlayDetails rasterOverlayDetails;
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

} // namespace earth_engine
