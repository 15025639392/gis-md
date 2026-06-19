#pragma once

#include "FrameState.h"
#include "PresentationTrace.h"
#include "../renderer/RenderCommand.h"

#include <memory>
#include <vector>

namespace earth_engine {

class Tileset;

struct ScenePresentationTraceInput {
    const FrameState& frameState;
    const Tileset* terrainTileset = nullptr;
    const std::vector<std::unique_ptr<Tileset>>& additionalTilesets;
    const RenderCommandList& renderCommands;
};

class ScenePresentationTraceBuilder {
public:
    static PresentationTrace build(const ScenePresentationTraceInput& input);
};

} // namespace earth_engine
