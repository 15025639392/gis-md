#pragma once

#include "FeatureStore.h"
#include "../core/geodesy/Cartographic.h"

#include <optional>

namespace earth_engine {

class Ellipsoid;

/// snap 候选(编辑基础接口:引擎只出查询,吸附策略/UX 归应用层)。
struct SnapCandidate {
    enum class Part { Vertex, Edge };

    FeatureId featureId = kInvalidFeatureId;
    Part part = Part::Vertex;
    int ringIndex = -1;
    /// Vertex = 顶点序号;Edge = 边起点序号(polygon 环闭合末边 = [n-1, 0])。
    int vertexIndex = -1;
    /// 吸附目标坐标(Vertex = 该顶点;Edge = 边上最近点)。
    Cartographic position;
    double distanceMeters = 0.0;
};

/// snap 候选查询(纯函数,无状态)。
///
/// 给定参考坐标 + 容差(米):R-tree 预筛 → ECEF 距离逐候选比较。
/// 顶点优先:容差内存在顶点 → 返回最近顶点;否则返回最近边上点。
/// 边上最近点按端点 lng/lat/h 线性插值(容差尺度公里内误差可忽略)。
class FeatureSnapQuery {
public:
    /// @param exclude 排除的要素(编辑中要素不吸附自己),kInvalidFeatureId=不排除
    static std::optional<SnapCandidate> nearest(
        const FeatureStore& store,
        const Ellipsoid& ellipsoid,
        const Cartographic& around,
        double toleranceMeters,
        FeatureId exclude = kInvalidFeatureId);
};

} // namespace earth_engine
