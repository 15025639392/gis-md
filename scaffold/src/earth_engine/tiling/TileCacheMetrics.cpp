#include "TileCacheMetrics.h"

#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"
#include "TileSurface.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "../providers/TerrainProvider.h"
#include "../renderer/RenderDevice.h"

#include <array>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace earth_engine {

int64_t TileCacheMetrics::estimateHeightmapBytes(
    const DecodedHeightmap& heightmap) {
    int64_t bytes = 0;
    bytes += static_cast<int64_t>(heightmap.rawData.size());
    bytes += static_cast<int64_t>(
        heightmap.heights.size() * sizeof(float));
    bytes += static_cast<int64_t>(
        heightmap.noDataValues.size() * sizeof(float));
    bytes += static_cast<int64_t>(
        heightmap.metadataAvailability.size() * sizeof(std::array<int, 5>));
    for (const auto& update : heightmap.quantizedMeshAvailabilityUpdates) {
        bytes += static_cast<int64_t>(
            sizeof(DecodedHeightmap::QuantizedMeshAvailabilityUpdate));
        bytes += static_cast<int64_t>(update.subtreeKey.schemeId.size());
        bytes += static_cast<int64_t>(
            update.metadataAvailability.size() * sizeof(std::array<int, 5>));
    }
    if (heightmap.surfaceMesh) {
        bytes += static_cast<int64_t>(
            heightmap.surfaceMesh->vertices.size() * sizeof(SurfaceVertex));
        bytes += static_cast<int64_t>(
            heightmap.surfaceMesh->indices.size() * sizeof(uint32_t));
        bytes += static_cast<int64_t>(
            heightmap.surfaceMesh->gpuVertices.size() *
            sizeof(SurfaceGpuVertex));
    }
    return bytes;
}

int64_t TileCacheMetrics::estimateTileBytes(const TilesetTile& tile) {
    int64_t bytes = 0;
    if (tile.mesh) {
        bytes += static_cast<int64_t>(
            tile.mesh->vertices.size() * sizeof(SurfaceVertex));
        bytes += static_cast<int64_t>(
            tile.mesh->indices.size() * sizeof(uint32_t));
        bytes += static_cast<int64_t>(tile.mesh->waterMask.data.size());
        bytes += static_cast<int64_t>(
            tile.mesh->metadataAvailability.size() *
            sizeof(std::array<int, 5>));
    }
    if (tile.gltfModel) {
        bytes += tile.gltfModel->byteSize();
    }
    if (tile.gpuVertexBuffer) {
        bytes += static_cast<int64_t>(tile.gpuVertexBuffer->size());
    }
    if (tile.gpuIndexBuffer) {
        bytes += static_cast<int64_t>(tile.gpuIndexBuffer->size());
    }
    for (const std::unique_ptr<Texture>& texture : tile.gltfTextureResources) {
        if (texture) {
            bytes += static_cast<int64_t>(
                texture->width() * texture->height() * 4);
        }
    }
    for (const GltfPrimitiveRenderResources& primitive :
         tile.gltfPrimitiveResources) {
        if (primitive.vertexBuffer) {
            bytes += static_cast<int64_t>(primitive.vertexBuffer->size());
        }
        if (primitive.indexBuffer) {
            bytes += static_cast<int64_t>(primitive.indexBuffer->size());
        }
    }
    if (tile.heightmap) {
        bytes += estimateHeightmapBytes(*tile.heightmap);
    }
    for (const auto& overlay : tile.rasterOverlays) {
        const std::shared_ptr<RasterOverlayTile> readyTile =
            overlay ? overlay->getReadyTileHandle() : nullptr;
        Texture* texture = readyTile ? readyTile->getTexture() : nullptr;
        if (!texture) {
            continue;
        }
#ifdef __ANDROID__
        // Android lifetime guard: this estimator runs after raster provider trimming.
        // Avoid crashing on retained raw pointers while logging the stale state.
        if (!RasterOverlayTileProvider::isLiveTextureForLifetimeGuard(texture)) {
            static int staleTextureLogCount = 0;
            if (staleTextureLogCount < 20) {
                __android_log_print(
                    ANDROID_LOG_WARN,
                    "EarthPerfCrash",
                    "stale raster texture in estimateTileBytes tile=%s/%d/%d/%d state=%d readyTile=%p texture=%p",
                    tile.key.schemeId.c_str(),
                    tile.key.z,
                    tile.key.x,
                    tile.key.y,
                    static_cast<int>(overlay->getState()),
                    static_cast<const void*>(readyTile.get()),
                    static_cast<void*>(texture));
                ++staleTextureLogCount;
            }
            continue;
        }
#endif
        bytes += static_cast<int64_t>(texture->width() *
                                      texture->height() * 4);
    }
    return bytes;
}

int64_t TileCacheMetrics::estimateTotalBytes(
    const std::unordered_map<
        std::string,
        std::unique_ptr<TilesetTile>>& tiles,
    const std::unordered_map<
        std::string,
        std::unique_ptr<DecodedHeightmap>>& terrainCache) {
    int64_t bytes = 0;
    for (const auto& [key, tile] : tiles) {
        (void)key;
        if (tile) {
            bytes += estimateTileBytes(*tile);
        }
    }
    for (const auto& [key, heightmap] : terrainCache) {
        (void)key;
        if (heightmap) {
            bytes += estimateHeightmapBytes(*heightmap);
        }
    }
    return bytes;
}

} // namespace earth_engine
