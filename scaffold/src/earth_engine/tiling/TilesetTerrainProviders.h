#pragma once

#include "../content/GltfContentProvider.h"
#include "../providers/TerrainProvider.h"

#include <memory>

namespace earth_engine {

class TilesetTerrainProviders {
public:
    TilesetTerrainProviders(
        std::unique_ptr<TerrainProvider> legacyHeightmapTerrainProvider,
        std::unique_ptr<TilesetContentProvider> contentProvider);

    TerrainProvider* legacyHeightmapTerrainProvider() {
        return legacyHeightmapTerrainProvider_.get();
    }
    TerrainProvider* legacyHeightmapTerrainProvider() const {
        return legacyHeightmapTerrainProvider_.get();
    }

    TilesetContentProvider* contentProvider() { return contentProvider_.get(); }
    TilesetContentProvider* contentProvider() const {
        return contentProvider_.get();
    }

    bool contentProviderOwnsTerrainQuadtree() const;
    bool usesLegacyHeightmapTerrainSurfacePath() const;
    bool hasTerrainQuadtree() const;

private:
    std::unique_ptr<TerrainProvider> legacyHeightmapTerrainProvider_;
    std::unique_ptr<TilesetContentProvider> contentProvider_;
};

} // namespace earth_engine
