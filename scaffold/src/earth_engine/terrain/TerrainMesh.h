#pragma once

#include "../globe/Globe.h"
#include "TerrainTile.h"
#include "../core/math/Vec3.h"
#include <vector>
#include <memory>

namespace earth_engine {

/// 地形网格生成器。
///
/// 接收 Globe 基础网格 + TerrainTile 高度图，
/// 沿椭球面法线位移顶点，生成地形网格。
///
/// Skirt（裙边）在 tile 边界生成下沉边缘，防止不同 tile 之间出现裂缝。
class TerrainMeshBuilder {
public:
    /// 从基础网格和地形瓦片生成地形网格。
    /// @param baseMesh Globe 生成的基础椭球网格
    /// @param terrainTile 地形高度图（可为 nullptr，则生成平坦网格）
    /// @param skirtHeightMeters skirt 下沉深度（米，默认 -50m）
    /// @return 顶点 + 索引的 GPU 就绪网格
    static GlobeMesh build(const GlobeMesh& baseMesh,
                           const TerrainTile* terrainTile,
                           float skirtHeightMeters = -50.0f);

    /// 仅位移顶点（无 skirt）
    static GlobeMesh buildDisplaced(const GlobeMesh& baseMesh,
                                     const TerrainTile* terrainTile);

private:
    /// 从 texcoord 计算采样高度
    static float sampleHeightFromTile(const TerrainTile* tile,
                                       float texU, float texV);
};

} // namespace earth_engine
