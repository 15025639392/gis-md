#pragma once

#include "../renderer/RenderCommand.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class Renderer;
struct TilesetTile;

struct GltfDrawCommandBuildContext {
    uint64_t frameNumber = 0;
    uint64_t generation = 0;
    float transitionOpacity = 1.0f;
    std::optional<std::array<float, 4>> surfaceClipUv;
};

struct GltfDrawCommandBuildTimings {
    double eligibilityMs = 0.0;
    double cacheRebuildMs = 0.0;
    double commandCopyMs = 0.0;
    double perFrameStateMs = 0.0;
    double rasterBindingMs = 0.0;
    int commandCount = 0;
    int cacheRebuildCount = 0;
};

struct GltfDrawCommandBuilder {
    static void build(Renderer& renderer,
                      TilesetTile& tile,
                      const std::vector<ActivatedRasterOverlay*>& overlays,
                      RenderCommandList& commands,
                      const GltfDrawCommandBuildContext& context,
                      GltfDrawCommandBuildTimings* timings = nullptr);
};

} // namespace earth_engine
