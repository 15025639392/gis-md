#pragma once

#include "../renderer/RenderCommand.h"

namespace earth_engine {

class Renderer;
class Tileset;

class TilesetRenderFrameFacade {
public:
    static void buildRenderCommands(Tileset& tileset,
                                    Renderer& renderer,
                                    RenderCommandList& commands);
};

} // namespace earth_engine
