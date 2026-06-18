#pragma once

#include "../renderer/RenderCommand.h"

#include <cstdint>

namespace earth_engine {

class Renderer;
struct TilesetTile;

struct GltfDrawCommandBuildContext {
    uint64_t frameNumber = 0;
    uint64_t generation = 0;
    float transitionOpacity = 1.0f;
};

struct GltfDrawCommandBuilder {
    static void build(Renderer& renderer,
                      TilesetTile& tile,
                      RenderCommandList& commands,
                      const GltfDrawCommandBuildContext& context);
};

} // namespace earth_engine
