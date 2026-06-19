#include "TileCacheMetrics.h"

#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "../providers/TerrainProvider.h"
#include "../renderer/RenderDevice.h"

namespace earth_engine {

int64_t TileCacheMetrics::estimateHeightmapBytes(
    const DecodedHeightmap& heightmap) {
    return TileRenderContentState::estimateHeightmapBytes(heightmap);
}

int64_t TileCacheMetrics::estimateTileBytes(const TilesetTile& tile) {
    int64_t bytes = tile.content.renderContent.estimateRetainedBytes();
    tile.rasterOverlayState.forEachMapping([&](const auto* overlay) {
        const std::shared_ptr<RasterOverlayTile> readyTile =
            overlay ? overlay->getReadyTileHandle() : nullptr;
        Texture* texture = readyTile ? readyTile->getTexture() : nullptr;
        if (!texture) {
            return;
        }
        bytes += static_cast<int64_t>(texture->width() *
                                      texture->height() * 4);
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
