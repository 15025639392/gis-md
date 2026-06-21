#pragma once

#include "../renderer/RenderCommand.h"

#include <cstdint>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
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
                      const std::vector<ActivatedRasterOverlay*>& overlays,
                      RenderCommandList& commands,
                      const GltfDrawCommandBuildContext& context);
};

} // namespace earth_engine
