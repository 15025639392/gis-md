#include "SceneFrameDiagnosticsAggregator.h"

#include "SceneRenderDiagnostics.h"
#include "SceneTilesetDiagnostics.h"
#include "../tiling/Tileset.h"

namespace earth_engine {

void SceneFrameDiagnosticsAggregator::aggregateRenderFrame(
    const RenderCommandList& commands,
    const Tileset* terrainTileset,
    const std::vector<std::unique_ptr<Tileset>>& additionalTilesets,
    Diagnostics& diagnostics) {
    SceneTilesetDiagnostics::reset(diagnostics);
    SceneRenderDiagnostics::addRenderCommands(commands, diagnostics);
    diagnostics.contentTilesets =
        static_cast<int>(additionalTilesets.size());

    if (terrainTileset) {
        SceneTilesetDiagnostics::addTileset(
            diagnostics,
            *terrainTileset,
            true);
    }
    for (const auto& tileset : additionalTilesets) {
        if (tileset) {
            SceneTilesetDiagnostics::addTileset(
                diagnostics,
                *tileset,
                false);
        }
    }

    SceneRenderDiagnostics::finalizeRenderCommandFields(diagnostics);
}

} // namespace earth_engine
