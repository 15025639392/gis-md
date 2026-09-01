#pragma once

#include "Feature.h"
#include "../core/math/Vec3.h"

#include <cstdint>
#include <vector>

namespace earth_engine {

class Ellipsoid;

/// 多边形 fill 镶嵌结果(GPU 就绪几何,ECEF)。
struct TessellatedFill {
    std::vector<Vec3> positions;           ///< ECEF 顶点(去重后唯一点)
    std::vector<uint32_t> fillIndices;     ///< 三角形索引(每 3 个,lng/lat 下 CCW)
    std::vector<uint32_t> outlineIndices;  ///< 描边线索引(每 2 个,环边界)
};

struct PolygonTessellationDiagnostics {
    double setupMs = 0.0;
    double globeDensifyMs = 0.0;
    double intersectionMs = 0.0;
    double cdtMs = 0.0;
    double cdtSuperTriangleMs = 0.0;
    double cdtPointInsertMs = 0.0;
    double cdtConstraintInsertMs = 0.0;
    double cdtExtractInsideMs = 0.0;
    double ecefMs = 0.0;
    size_t inputPoints = 0;
    size_t initialConstraints = 0;
    size_t densifiedPoints = 0;
    size_t intersectionConstraints = 0;
    size_t intersectionPairs = 0;
    size_t intersectionCandidatePairs = 0;
    size_t triangleCount = 0;
    size_t cdtPointTriangleTests = 0;
    size_t cdtPointBadTriangles = 0;
    size_t cdtConstraintEdgeTests = 0;
    size_t cdtConstraintCrossTests = 0;
    size_t cdtConstraintsAlreadyPresent = 0;
    size_t cdtConstraintsInserted = 0;
    size_t cdtPeakTriangles = 0;
    size_t cdtPointCapacityGrowths = 0;
    size_t cdtTriangleCapacityGrowths = 0;
};

/// 多边形 fill 镶嵌器。
///
/// Feature(polygon,rings[0]=外环 rings[1..]=孔)→ 受约束 Delaunay 三角化 → ECEF
/// 顶点 + fill 三角形 + outline 线。三角化在 (lng,lat) 2D 空间做(弧度尺度条件数好);
/// 顶点几何在 ECEF。
///
/// 处理:闭合环去重末点、全局近重合点去重(编辑可能产生)、退化(点数不足)返回空。
/// 高度取各顶点 cartographic 高 + heightOffset;贴地钳制由调用方预变换顶点高度。
///
/// `maxEdgeMeters > 0` 时做地球网格补全(VectorFill 斜视射线的根修):
/// 约束边按椭球弦长细分(ECEF 线性插值再投影回椭球),并在面内撒网格
/// Steiner,避免 CDT 对角线跨过整个面。0 = 关闭(旧测试/小面)。
class PolygonTessellator {
public:
    /// @param steinerPoints 可选内部散点(P3 贴地:面内网格采样点,CDT 把
    ///        它们连进三角网让 fill 跟随地形起伏)。须落在多边形内部;
    ///        环外的点会被 flood-fill 自然排除,不产生错误三角形。
    /// @param maxEdgeMeters ECEF 边长上限(米)。≤0 不细分。
    static TessellatedFill tessellate(
        const Feature& feature,
        const Ellipsoid& ellipsoid,
        double heightOffset = 0.0,
        const std::vector<Cartographic>* steinerPoints = nullptr,
        double maxEdgeMeters = 0.0,
        PolygonTessellationDiagnostics* diagnostics = nullptr);
};

} // namespace earth_engine
