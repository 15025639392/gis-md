#pragma once

#include "../providers/TerrainProvider.h"
#include "../tiling/SurfaceTile.h"
#include "../core/math/Rectangle.h"
#include "../core/math/Vec3.h"
#include <vector>
#include <array>
#include <cstdint>
#include <cstring>

namespace earth_engine {

/// Parsed Quantized-Mesh-1.0 terrain tile.
///
/// Format specification: https://github.com/CesiumGS/quantized-mesh
class QuantizedMeshParser {
public:
    struct Header {
        double centerX = 0, centerY = 0, centerZ = 0;
        double minimumHeight = 0, maximumHeight = 0;
        double boundingSphereX = 0, boundingSphereY = 0, boundingSphereZ = 0;
        double boundingSphereRadius = 0;
        double horizonOcclusionX = 0, horizonOcclusionY = 0, horizonOcclusionZ = 0;
        uint32_t vertexCount = 0;
    };

    /// Decoded Quantized-Mesh terrain payload, before choosing a renderer
    /// representation. This mirrors cesium-native's QuantizedMeshLoader path:
    /// decode the terrain content once, then let the content loader build a
    /// glTF terrain model without routing through SurfaceTileMesh.
    struct DecodedTile {
        std::vector<SurfaceVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<SurfaceGpuVertex> gpuVertices;
        Vec3 localOriginEcef = Vec3::zero();
        double minimumHeight = 0.0;
        double maximumHeight = 0.0;
        Vec3 horizonOcclusionPoint = Vec3::zero();
        SkirtMetadata skirtMetadata;
        WaterMask waterMask;
        bool hasMetadataAvailability = false;
        std::vector<QuantizedMeshAvailabilityRange> metadataAvailability;
        RasterOverlayDetails rasterOverlayDetails;
    };

    /// Decode Quantized-Mesh-1.0 terrain without selecting a renderer-facing
    /// mesh container. Returns nullptr on malformed input.
    static std::unique_ptr<DecodedTile> parseToDecodedTile(
        const uint8_t* data, size_t len,
        const Rectangle& bounds,
        bool enableWaterMask = false);

    /// Directly convert to a SurfaceTileMesh (retains the optimized
    /// triangulation from QuantizedMesh). This is a wrapper over
    /// parseToDecodedTile for native surface fallbacks and parser tests.
    /// Returns nullptr on error.
    /// @param bounds geographic bounds of the tile in radians
    static std::unique_ptr<SurfaceTileMesh> parseToSurfaceTileMesh(
        const uint8_t* data, size_t len,
        const Rectangle& bounds,
        bool enableWaterMask = false);

    /// cesium-native QuantizedMeshLoader::loadMetadata equivalent.
    /// Parses only Quantized Mesh extension ID=4 availability rectangles.
    /// Each entry: {levelOffset, startX, startY, endX, endY}.
    static std::vector<QuantizedMeshAvailabilityRange> parseMetadataAvailability(
        const uint8_t* data, size_t len);

private:
    static int32_t zigZagDecode(int32_t value) noexcept {
        return (value >> 1) ^ (-(value & 1));
    }
};

} // namespace earth_engine
