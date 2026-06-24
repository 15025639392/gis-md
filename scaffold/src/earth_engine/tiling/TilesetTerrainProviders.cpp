#include "TilesetTerrainProviders.h"

namespace earth_engine {

TilesetTerrainProviders::TilesetTerrainProviders(
    std::unique_ptr<TerrainProvider> legacyHeightmapTerrainProvider,
    std::unique_ptr<TilesetContentProvider> contentProvider)
    : legacyHeightmapTerrainProvider_(
          contentProvider && contentProvider->providesTerrainQuadtree()
              ? std::unique_ptr<TerrainProvider>{}
              : std::move(legacyHeightmapTerrainProvider)),
      contentProvider_(std::move(contentProvider)) {}

bool TilesetTerrainProviders::contentProviderOwnsTerrainQuadtree() const {
    return contentProvider_ && contentProvider_->providesTerrainQuadtree();
}

bool TilesetTerrainProviders::usesLegacyHeightmapTerrainSurfacePath() const {
    return legacyHeightmapTerrainProvider_ != nullptr;
}

bool TilesetTerrainProviders::hasTerrainQuadtree() const {
    return usesLegacyHeightmapTerrainSurfacePath() ||
           contentProviderOwnsTerrainQuadtree();
}

} // namespace earth_engine
