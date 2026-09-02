#pragma once

#include "../renderer/RenderCommand.h"
#include "TerrainEdgeLutTable.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace earth_engine {

class RasterOverlayFrameContext;
class Renderer;
class TileRenderContentState;
struct TilesetTile;

/// 把一个瓦片的渲染内容分类成绘制用的地表来源(fill proxy / 椭球回落 /
/// 真实地形 / 未知)。B2a 门② 页面 determination 复用它筛「真实地形」瓦片,
/// 与 draw 命令构建走同一判定(单一事实源,勿复制粘贴逻辑)。
TerrainSurfaceCommandSource terrainSurfaceSourceForDraw(
    const TileRenderContentState& renderContent);

struct GltfDrawCommandBuildContext {
    const RasterOverlayFrameContext& rasterFrame;
    uint64_t frameNumber = 0;
    uint64_t generation = 0;
    float transitionOpacity = 1.0f;
    std::optional<std::array<float, 4>> surfaceClipUv;
    // 机制 A(祖先高度重映射):surfaceClipUv 所属的后代瓦片。非空且模板/高度
    // 纹理就绪时,盖章期把命令换成"后代模板几何 + 祖先高度子矩形采样"(真边
    // 真裙墙,无 discard 切缝);任一资源未就绪回落旧 discard 裁剪(mode 1)。
    const TilesetTile* surfaceClipDescendant = nullptr;
    // ①-1(A′):本帧边高度差表(TilePlan::edgeLutTables,resolve 阶段建成的
    // 纯数据)。draw 按 terrainEdgeCellKey(tile.key) 查表,不再触碰任何瓦片/
    // entry 指针。nullptr = 无表(该 tileset 无地形吸附路径)。
    const TerrainEdgeLutTableMap* edgeLutTables = nullptr;
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
                      RenderCommandList& commands,
                      const GltfDrawCommandBuildContext& context,
                      GltfDrawCommandBuildTimings* timings = nullptr);
};

} // namespace earth_engine
