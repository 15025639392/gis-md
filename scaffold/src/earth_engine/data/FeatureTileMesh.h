#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "../core/math/Vec3.h"

namespace earth_engine {

/// stencil 分类体的 CPU 侧按色分组(key = RGBA8 打包)。
///
/// 放在 data/ 与 FeatureTileMesh 同层,是因为 worker 现在也产出它(贴地
/// 瓦片走 stencil 双 pass):生产方在 data/,消费方 FeatureRenderLayer 在
/// layers/,载荷类型必须在下层,否则 data → layers 反向依赖。
///
/// **按色而不是按要素分组**:同色要素的顶点/索引取并集进同一组,一组一对
/// draw(体 pass + 色 pass)。所以 draw call 随**颜色种类数**增长,不随
/// 要素数增长 —— 底图万级要素能走这条路的前提就在这里。
struct VolumeCpuGroup {
    std::array<float, 4> color{0, 0, 0, 1};
    std::vector<float> verts;
    std::vector<uint32_t> indices;
};
using VolumeCpuGroups = std::map<uint32_t, VolumeCpuGroup>;

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
/// point/label 需要图集(必须渲染线程),留在 store 路径。
struct FeatureTileMesh {
    Vec3 origin = Vec3::zero();
    bool hasOrigin = false;
    std::vector<float> fillVerts;
    std::vector<uint32_t> fillIndices;
    std::vector<float> lineVerts;
    std::vector<uint32_t> lineIndices;
    /// 贴地(ClampToGround + 后端支持 stencil 分类)时,fill/line 改产
    /// 挤出体走双 pass 像素级贴合;此时上面的 fill/lineVerts 为空(两条路
    /// **互斥**,同时产出会让同一份内容画两遍)。
    VolumeCpuGroups fillVolumeGroups;
    VolumeCpuGroups lineVolumeGroups;

    bool empty() const {
        return fillIndices.empty() && lineIndices.empty() &&
               fillVolumeGroups.empty() && lineVolumeGroups.empty();
    }
};

} // namespace earth_engine
