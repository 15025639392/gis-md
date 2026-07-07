#pragma once

#include "Feature.h"
#include "../core/math/Vec3.h"

#include <cstdint>
#include <vector>

namespace earth_engine {

class Ellipsoid;

/// 线 ribbon 顶点(屏幕空间线宽,挤出在顶点着色器完成)。
/// 见设计 §6.2:屏幕垂向视角相关,不能预烘焙,故携带相邻位置由 shader 现算。
struct LineVertex {
    Vec3 pos;            ///< 本顶点 ECEF
    Vec3 prev;           ///< 前驱 ECEF(端点哨兵 = pos)
    Vec3 next;           ///< 后继 ECEF(端点哨兵 = pos)
    float side;          ///< +1 / -1 挤出方向
    float lengthSoFar;   ///< 沿线累计 3D 弧长(m),供 dash
};

/// 线镶嵌结果(GPU 就绪 ribbon)。
struct TessellatedLine {
    std::vector<LineVertex> vertices;   ///< 每折线顶点 2 个(side ±1)
    std::vector<uint32_t> indices;      ///< 每段 2 三角形
};

/// 线镶嵌器。
///
/// Feature(LineString,rings[0]=顶点序列)→ ribbon 顶点 + 三角形索引。产出 prev/next/
/// side/lengthSoFar 布局,供 §6.2 的顶点着色器现算屏幕垂向 + miter join。
///
/// 拓扑:n 顶点(去重后)→ 2n 顶点、6·段数 索引(open 段数 n-1,closed n)。
/// 端点 prev/next 用 pos 自身作哨兵(shader 据此退化为单段方向 + cap)。
/// 几何/属性可单测;屏幕挤出的视觉正确性属 shader,留真机验证。
class LineTessellator {
public:
    /// @param closed 首尾顶点是否环绕相连(polygon outline 用 true;LineString 用 false)
    static TessellatedLine tessellate(const Feature& feature,
                                      const Ellipsoid& ellipsoid,
                                      double heightOffset = 0.0,
                                      bool closed = false);
};

} // namespace earth_engine
