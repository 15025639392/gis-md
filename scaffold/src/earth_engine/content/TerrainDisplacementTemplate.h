#pragma once

#include <cstdint>
#include <vector>

#include "../core/math/Mat4.h"
#include "../core/math/Rectangle.h"
#include "../core/math/Vec3.h"

namespace earth_engine {

// 北极星 Phase 2c（地形 GPU 位移）P1a：共享位移模板。
//
// 模板 = 零高程、贴椭球的规则栅格，顶点存在**瓦片中心 ENU 局部帧**里。
// 关键性质：ENU 帧随经度旋转，所以对同一 {LOD, mercator-row}（同纬度带、
// 同经度宽度）的所有瓦片，局部位置/法线**逐列不变**（经度平移 = 绕 Z 旋转，
// 被瓦片自己的 ENU 帧抵消）——因此一份模板可跨该行所有列共享，令地形 VBO
// 字节有界（北极星 §5 闸门）。每瓦片的经纬度落位由 per-tile 刚体变换
// enuToEcef(tileCenter) 承担（折进 MVP，双精度），不进模板。
//
// 与现 CPU 烘焙路径（EllipsoidTerrainMeshBuilder）的关系：采样约定完全一致
// （纬度 north(v=0)→south(v=1)、经度 longitudeAt 反经线安全、索引缠绕
// a,c,b,b,c,d），所以模板 + per-tile 高度纹理位移是现路径的等价替换。
// 位移分解 world = frame·(localPos + localNormal·height) 与
// cartographicToCartesian(lng,lat,height) 代数等价（见 P0 test_terrain_gpu_
// displacement_precision + redesign doc §5）。

struct TerrainDisplacementTemplateVertex {
    float localPos[3];     // ENU 帧（原点=瓦片中心椭球面点），零高程面点
    float localNormal[3];  // ENU 帧大地法线（位移方向；模板内逐列不变）
    float uv[2];           // 均匀 [0,1] texcoord（overlay 用）
};

struct TerrainDisplacementTemplate {
    int gridSize = 0;  // 单元格数；顶点行/列数 n = gridSize + 1
    std::vector<TerrainDisplacementTemplateVertex> vertices;  // n*n，行主序
    std::vector<uint32_t> indices;                            // gridSize²*6
};

// 为一块瓦片（其纬度范围 + 经度宽度决定形状）生成共享位移模板。
// 同 {LOD, mercator-row} 的瓦片得到逐值相等的 vertices（列无关）。
TerrainDisplacementTemplate buildTerrainDisplacementTemplate(
    const Rectangle& tileBounds, int gridSize);

// 瓦片的 ENU→ECEF 刚体帧（= enuToEcef(瓦片中心)）。调用方折进模型矩阵（双精度
// compose 后降 f32），承担该瓦片的经纬度落位。模板 localPos 就是相对此帧的坐标。
Mat4 terrainTemplateTileFrame(const Rectangle& tileBounds);

// 重建某顶点在给定高程下的 ECEF 世界位置（= frame·(localPos + localNormal·h)）。
// 供 host 测试对拍与后续高度查询用。双精度算，不降 f32。
Vec3 reconstructTemplateWorldPosition(
    const TerrainDisplacementTemplate& tmpl, const Mat4& tileFrame,
    int vertexIndex, double height);

}  // namespace earth_engine
