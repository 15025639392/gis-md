#pragma once

#include "Diagnostics.h"
#include "../renderer/RenderCommand.h"

#include <memory>
#include <vector>

namespace earth_engine {

class Tileset;

class SceneFrameDiagnosticsAggregator {
public:
    static void aggregateRenderFrame(
        const RenderCommandList& commands,
        const Tileset* terrainTileset,
        const std::vector<std::unique_ptr<Tileset>>& additionalTilesets,
        Diagnostics& diagnostics);
};

} // namespace earth_engine
