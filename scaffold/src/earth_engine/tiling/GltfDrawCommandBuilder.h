#pragma once

#include "../renderer/RenderCommand.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class Renderer;
class TileRenderContentState;
struct TilesetTile;

/// 把一个瓦片的渲染内容分类成绘制用的地表来源(fill proxy / 椭球回落 /
/// 真实地形 / 未知)。B2a 门② 页面 determination 复用它筛「真实地形」瓦片,
/// 与 draw 命令构建走同一判定(单一事实源,勿复制粘贴逻辑)。
TerrainSurfaceCommandSource terrainSurfaceSourceForDraw(
    const TileRenderContentState& renderContent);

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
