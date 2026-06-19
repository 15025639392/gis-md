#pragma once

#include "TileRenderFrameContext.h"
#include "../renderer/RenderCommand.h"

namespace earth_engine {

class Renderer;

class TilesetRenderFrameExecutor {
public:
    static void buildRenderCommands(TileRenderFrameContext context,
                                    Renderer& renderer,
                                    RenderCommandList& commands);
};

} // namespace earth_engine
