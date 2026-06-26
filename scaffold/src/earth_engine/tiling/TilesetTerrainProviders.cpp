#include "TilesetTerrainProviders.h"

namespace earth_engine {

TilesetTerrainProviders::TilesetTerrainProviders(
    std::unique_ptr<TilesetContentProvider> contentProvider)
    : contentProvider_(std::move(contentProvider)) {}

bool TilesetTerrainProviders::contentProviderOwnsTerrainQuadtree() const {
    return contentProvider_ && contentProvider_->providesTerrainQuadtree();
}

bool TilesetTerrainProviders::hasTerrainQuadtree() const {
    return contentProviderOwnsTerrainQuadtree();
}

} // namespace earth_engine
