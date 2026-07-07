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

/// 多边形 fill 镶嵌器。
///
/// Feature(polygon,rings[0]=外环 rings[1..]=孔)→ 受约束 Delaunay 三角化 → ECEF
/// 顶点 + fill 三角形 + outline 线。三角化在 (lng,lat) 2D 空间做(弧度尺度条件数好);
/// 顶点几何在 ECEF。
///
/// 处理:闭合环去重末点、全局近重合点去重(编辑可能产生)、退化(点数不足)返回空。
/// 高度取各顶点 cartographic 高 + heightOffset(Absolute);贴地钳制属后续子系统。
class PolygonTessellator {
public:
    static TessellatedFill tessellate(const Feature& feature,
                                      const Ellipsoid& ellipsoid,
                                      double heightOffset = 0.0);
};

} // namespace earth_engine
