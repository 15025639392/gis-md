#pragma once

#include <memory>

namespace earth_engine {

class TilesetContentProvider;

std::unique_ptr<TilesetContentProvider>
decorateAmapClassicTerrainContentProvider(
    std::unique_ptr<TilesetContentProvider> provider);

} // namespace earth_engine
