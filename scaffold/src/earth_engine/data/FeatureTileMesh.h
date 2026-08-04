#pragma once

#include <cstdint>
#include <vector>

#include "../core/math/Vec3.h"

namespace earth_engine {

/// 一块瓦片镶嵌后的 CPU 顶点/索引(E1 worker 全链镶嵌的产物)。
///
/// 放在 data/ 而非 layers/ 是为了分层:生产方 MvtVectorSource 在 data/,
/// 消费方 FeatureRenderLayer 在 layers/,把载荷类型放在下层两边都能用,
/// 避免 data → layers 的反向依赖。本结构只依赖 Vec3 与标准容器。
///
/// 顶点是**相对 origin 的 float**(origin 为 double ECEF),与 store 路径的
/// 桶原点 RTE 约定一致 —— 每帧以 double 算 mvp = viewProj·translate(origin)
/// 后降 float,消除 ECEF 直存 float 的米级抖动。
///
/// v1 只含 fill/line:point/label 需要图集(必须渲染线程),留在 store 路径。
struct FeatureTileMesh {
    Vec3 origin = Vec3::zero();
    bool hasOrigin = false;
    std::vector<float> fillVerts;
    std::vector<uint32_t> fillIndices;
    std::vector<float> lineVerts;
    std::vector<uint32_t> lineIndices;

    bool empty() const { return fillIndices.empty() && lineIndices.empty(); }
};

} // namespace earth_engine
