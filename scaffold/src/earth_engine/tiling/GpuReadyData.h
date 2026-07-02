#pragma once

#include "TileRenderContentState.h"
#include "../core/math/Vec3.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace earth_engine {

/// CPU-prepared data ready for GPU upload.
/// Built on worker thread, consumed on main thread (GL context).
struct GpuReadyPrimitive {
    // Vertex data (already converted from SurfaceVertex)
    std::vector<uint8_t> vertexBytes;
    size_t vertexStride = 0;   // 32 (TerrainGpuVertex) or 120 (GltfGpuVertex)
    size_t vertexCount = 0;

    // Index data
    std::vector<uint32_t> indices;
    size_t indexCount = 0;

    // Texture data (decoded to RGBA8/R8)
    struct TextureData {
        std::vector<uint8_t> pixels;
        int width = 0;
        int height = 0;
        int channels = 4;  // 1=R8, 4=RGBA8
        bool mipmap = false;
    };
    std::vector<TextureData> textures;

    // Metadata (material properties, texture bindings, etc.)
    // Populated by CPU phase, vertexBuffer/indexBuffer/texture filled by GPU phase.
    GltfPrimitiveRenderResources metadata;

    // Sort center for translucent sorting
    Vec3 sortCenterEcef = Vec3::zero();

    // Instance data (for instanced rendering)
    struct InstanceData {
        std::vector<uint8_t> bytes;
        size_t count = 0;
        size_t stride = 0;
    };
    std::optional<InstanceData> instances;
};

/// Collection of CPU-prepared primitives for a single tile.
struct GpuReadyData {
    std::vector<GpuReadyPrimitive> primitives;
    bool valid() const { return !primitives.empty(); }
};

} // namespace earth_engine