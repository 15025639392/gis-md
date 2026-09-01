#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <utility>
#include <vector>

namespace earth_engine {

struct ConstrainedDelaunayDiagnostics {
    double superTriangleMs = 0.0;
    double pointInsertMs = 0.0;
    double constraintInsertMs = 0.0;
    double extractInsideMs = 0.0;
    size_t pointTriangleTests = 0;
    size_t pointBadTriangles = 0;
    size_t constraintEdgeLookups = 0;
    size_t constraintCrossTriangleTests = 0;
    size_t constraintsAlreadyPresent = 0;
    size_t constraintsInserted = 0;
    size_t peakTriangles = 0;
    size_t initialPointCapacity = 0;
    size_t initialTriangleCapacity = 0;
    size_t pointCapacityGrowths = 0;
    size_t triangleCapacityGrowths = 0;
};

/// 受约束 Delaunay 三角化(CDT)。
///
/// 输入 2D 点集 + 约束边(须出现在结果中的边,如多边形/孔洞边界),输出**落在约束
/// 区域内部**的三角形(索引三元组,指向输入点)。
///
/// 相比 ear-clipping/earcut:CDT 稳健(约束边强制保留)、三角形质量好(无退化细长
/// 三角,尽量满足空外接圆 Delaunay 性质),适合编辑场景的畸形/凹/带孔多边形。
///
/// 算法:Bowyer-Watson 增量 Delaunay(brute-force cavity) → Anglada 约束边插入
/// (移除被约束边穿越的三角形,对两侧伪多边形递归重三角化) → 邻接 flood-fill 按
/// 约束边奇偶标记内/外(自动挖空孔洞,winding 无关)。
///
/// **前置约定**:输入点两两不同(调用方须先 dedup 合并重合点);约束边端点为有效
/// 点索引且不自交。predicates 用 double —— 对 lng/lat 弧度尺度(小量、条件数好)
/// 足够;极区/大范围的退化稳健性(adaptive predicates)留作后续。
class ConstrainedDelaunay {
public:
    using Edge = std::pair<uint32_t, uint32_t>;

    /// 三角化。返回内部三角形的展平索引(每 3 个 = 一个三角形,CCW,指向 points)。
    /// 点数 < 3 或退化返回空。
    static std::vector<uint32_t> triangulate(
        const std::vector<glm::dvec2>& points,
        const std::vector<Edge>& constraintEdges,
        ConstrainedDelaunayDiagnostics* diagnostics = nullptr);
};

} // namespace earth_engine
