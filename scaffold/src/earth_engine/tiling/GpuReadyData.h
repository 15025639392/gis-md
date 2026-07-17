#pragma once

#include "TileRenderContentState.h"
#include "../content/GltfModel.h"
#include "../core/math/Vec3.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

namespace earth_engine {

/// CPU-prepared data ready for GPU upload.
/// Built on worker thread, consumed on main thread (GL context).
struct GpuReadyPrimitive {
    // Vertex data (already converted from SurfaceVertex)
    std::vector<uint8_t> vertexBytes;
    size_t vertexStride = 0;   // 40 (TerrainGpuVertex) or 120 (GltfGpuVertex)
    size_t vertexCount = 0;

    // Index data, already in the exact byte layout uploaded to the GPU.
    // indexByteSize is 2 (uint16) when every index fits below the GLES3
    // fixed primitive-restart sentinel 0xFFFF, else 4 (uint32).
    std::vector<uint8_t> indexBytes;
    size_t indexByteSize = sizeof(uint32_t);
    size_t indexCount = 0;

    // Converts model-space uint32 indices into the narrowest GPU upload
    // format. vertexCount bounds the index range: indices are < vertexCount,
    // so vertexCount <= 0xFFFF guarantees the max index stays below the
    // 0xFFFF restart sentinel.
    void assignIndices(const std::vector<uint32_t>& indices,
                       size_t vertexCount) {
        indexCount = indices.size();
        if (vertexCount > 0 && vertexCount <= 0xFFFFu) {
            indexByteSize = sizeof(uint16_t);
            indexBytes.resize(indexCount * sizeof(uint16_t));
            uint16_t* out =
                reinterpret_cast<uint16_t*>(indexBytes.data());
            for (size_t i = 0; i < indexCount; ++i) {
                out[i] = static_cast<uint16_t>(indices[i]);
            }
            return;
        }
        indexByteSize = sizeof(uint32_t);
        indexBytes.resize(indexCount * sizeof(uint32_t));
        std::memcpy(
            indexBytes.data(),
            indices.data(),
            indexBytes.size());
    }

    // Metadata (material properties, texture bindings, etc.)
    // Populated by CPU phase, vertexBuffer/indexBuffer/texture filled by GPU phase.
    GltfPrimitiveRenderResources metadata;

    // Texture references retain model texture-slot semantics until the main
    // thread has created the corresponding GPU textures.
    std::optional<size_t> terrainWaterMaskTextureIndex;
    std::optional<GltfTextureBinding> baseColorTexture;
    std::optional<GltfTextureBinding> metallicRoughnessTexture;
    std::optional<GltfTextureBinding> anisotropyTexture;
    std::optional<GltfTextureBinding> specularTexture;
    std::optional<GltfTextureBinding> specularColorTexture;
    std::optional<GltfTextureBinding> specularGlossinessTexture;
    std::optional<GltfTextureBinding> transmissionTexture;
    std::optional<GltfTextureBinding> clearcoatTexture;
    std::optional<GltfTextureBinding> clearcoatRoughnessTexture;
    std::optional<GltfTextureBinding> clearcoatNormalTexture;
    std::optional<GltfTextureBinding> sheenColorTexture;
    std::optional<GltfTextureBinding> sheenRoughnessTexture;
    std::optional<GltfTextureBinding> normalTexture;
    std::optional<GltfTextureBinding> occlusionTexture;
    std::optional<GltfTextureBinding> emissiveTexture;

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
    Vec3 localOrigin = Vec3::zero();

    // Texture data (decoded to RGBA8/R8). Model-level: glTF textures are
    // shared across primitives, so they are decoded/uploaded once per tile
    // (P2-12) and bound per primitive by index in metadata.
    struct TextureData {
        std::vector<uint8_t> pixels;
        int width = 0;
        int height = 0;
        int channels = 4;  // 1=R8, 4=RGBA8
        bool mipmap = false;
        GltfTextureFilter minFilter = GltfTextureFilter::Linear;
        GltfTextureFilter magFilter = GltfTextureFilter::Linear;
        GltfTextureWrap wrapS = GltfTextureWrap::ClampToEdge;
        GltfTextureWrap wrapT = GltfTextureWrap::ClampToEdge;

        bool valid() const {
            if (width <= 0 || height <= 0 ||
                (channels != 1 && channels != 4)) {
                return false;
            }
            const size_t expectedBytes =
                static_cast<size_t>(width) *
                static_cast<size_t>(height) *
                static_cast<size_t>(channels);
            return pixels.size() == expectedBytes;
        }
    };
    std::vector<TextureData> textures;

    bool valid() const { return !primitives.empty(); }

    int64_t byteSize() const {
        int64_t bytes = 0;
        for (const GpuReadyPrimitive& primitive : primitives) {
            bytes += static_cast<int64_t>(primitive.vertexBytes.size());
            bytes += static_cast<int64_t>(primitive.indexBytes.size());
            if (primitive.instances) {
                bytes += static_cast<int64_t>(
                    primitive.instances->bytes.size());
            }
            if (primitive.metadata.vertexBuffer) {
                bytes += static_cast<int64_t>(
                    primitive.metadata.vertexBuffer->size());
            }
            if (primitive.metadata.indexBuffer) {
                bytes += static_cast<int64_t>(
                    primitive.metadata.indexBuffer->size());
            }
            if (primitive.metadata.instanceBuffer) {
                bytes += static_cast<int64_t>(
                    primitive.metadata.instanceBuffer->size());
            }
        }
        for (const TextureData& texture : textures) {
            bytes += static_cast<int64_t>(texture.pixels.size());
        }
        return bytes;
    }
};

} // namespace earth_engine
