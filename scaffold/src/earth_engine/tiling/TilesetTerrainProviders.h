#pragma once

#include "../content/GltfContentProvider.h"

#include <memory>

namespace earth_engine {

class TilesetTerrainProviders {
public:
    explicit TilesetTerrainProviders(
        std::unique_ptr<TilesetContentProvider> contentProvider);

    TilesetContentProvider* contentProvider() { return contentProvider_.get(); }
    const TilesetContentProvider* contentProvider() const {
        return contentProvider_.get();
    }

    bool contentProviderOwnsTerrainQuadtree() const;
    bool hasTerrainQuadtree() const;

private:
    std::unique_ptr<TilesetContentProvider> contentProvider_;
};

} // namespace earth_engine
