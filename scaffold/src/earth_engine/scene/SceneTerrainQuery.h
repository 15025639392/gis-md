#pragma once

#include "../camera/CameraController.h"
#include "../core/math/Vec3.h"

#include <functional>
#include <optional>
#include <vector>

namespace earth_engine {

class Tileset;

class SceneTerrainQuery {
public:
    static std::function<float(double, double)> makeLngLatHeightSampler(
        const Tileset* terrainTileset);
    // nullopt = 无地形数据(未加载 / 无覆盖瓦片 / 无地形 tileset),供相机 clamp
    // 区分真实海平面 0 与"未知",避免在未加载高地形处下沉。
    static std::optional<double> sampleHeight(const Tileset* terrainTileset,
                                              const Vec3& ecefPosition);
    // 相机近场探针的区域批量采样(TerrainAreaSampleFunc 的实现):以 groundEcef
    // 为中心构造经纬矩形(跨反经线拆分),一次收集候选瓦片后逐偏移点采样。
    // 渲染网格一致采样(createAreaHeightSampler 内建)——相机碰撞要的是
    // "不穿上屏那张面"。out 与 offsets 等长,无覆盖点 valid=false(候选集
    // 含覆盖矩形的粗层级瓦片,细瓦片未载时自然回退祖先高度,无需独立祖先链)。
    static void sampleAreaHeights(
        const Tileset* terrainTileset,
        const Vec3& groundEcef,
        double radiusMeters,
        const std::vector<glm::dvec2>& localOffsetsMeters,
        std::vector<CameraController::TerrainSample>& out);
};

} // namespace earth_engine
