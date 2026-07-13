#include "TileCacheMetrics.h"

#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "../providers/TerrainProvider.h"
#include "../renderer/RenderDevice.h"

#include <unordered_set>

namespace earth_engine {

namespace {

int64_t estimateTextureBytes(const Texture& texture) {
    return static_cast<int64_t>(texture.width()) *
           static_cast<int64_t>(texture.height()) * 4;
}

template <typename CountTextureFn>
int64_t estimateRasterMappingBytes(const TilesetTile& tile,
                                   CountTextureFn&& countTexture) {
    int64_t bytes = 0;
    tile.rasterOverlayState.forEachMapping([&](const auto* overlay) {
        const std::shared_ptr<RasterOverlayTile> readyTile =
            overlay ? overlay->getReadyTileHandle() : nullptr;
        Texture* texture = readyTile ? readyTile->getTexture() : nullptr;
        if (!texture) {
            return;
        }
        bytes += countTexture(*texture);
    });
    return bytes;
}

} // namespace

int64_t TileCacheMetrics::estimateHeightmapBytes(
    const DecodedHeightmap& heightmap) {
    return TileRenderContentState::estimateHeightmapBytes(heightmap);
}

int64_t TileCacheMetrics::estimateTileBytes(const TilesetTile& tile) {
    int64_t bytes = tile.content.renderContent.estimateRetainedBytes();
    std::unordered_set<const Texture*> countedTextures;
    bytes += estimateRasterMappingBytes(
        tile,
        [&countedTextures](const Texture& texture) {
            return countedTextures.insert(&texture).second
                ? estimateTextureBytes(texture)
                : 0;
        });
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
    std::unordered_set<const Texture*> countedRasterTextures;
    for (const auto& [key, tile] : tiles) {
        (void)key;
        if (tile) {
            bytes += tile->content.renderContent.estimateRetainedBytes();
            bytes += estimateRasterMappingBytes(
                *tile,
                [&countedRasterTextures](const Texture& texture) {
                    return countedRasterTextures.insert(&texture).second
                        ? estimateTextureBytes(texture)
                        : 0;
                });
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
