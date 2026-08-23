#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
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

/// 瓦片点符号实例(worker 产出的中间表)。
///
/// 点符号**不在 worker 侧定型 quad**:图集查找(位图帧 uv)是渲染线程状态。
/// worker 把纯计算部分全部做完(锚点投影、表达式求值出颜色/图形名、属性
/// 抽取),渲染线程准入时只做「图集解析 + 展开 4 顶点」——一瓦一次,非逐帧。
struct TileSymbolCpu {
    /// 锚点大地坐标(radian/meter,**原始几何高**,未含样式 offset)。
    /// 存经纬度而非 ECEF:贴地模式的地形采样是渲染线程状态,准入定型时
    /// 才能把锚点落到地面 —— worker 侧给 ECEF 就把高度焊死在椭球面了
    /// (山地会整批埋进地形被遮挡判定吃掉)。
    double lonRad = 0.0;
    double latRad = 0.0;
    double heightM = 0.0;
    float colorPacked = 0.0f;  ///< RGBA8 打包(worker 已求值样式表达式)
    int rank = 6;              ///< 数据侧重要度(小=重要),准入截断依据
    std::string icon;          ///< 已求值图形名(内置形状名或图集帧名,可空)
    std::string name;          ///< 标签文字(文字刀期用,先携带免二次解码)
};

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
    /// E 方案 P2:线 ribbon 的钳高源(每 ribbon 顶点 3 float:lon/lat 弧度 +
    /// colorPacked)。worker 无地形采样时只产椭球面高度;渲染线程 commit/
    /// 重钳时按 (lon,lat) 同源采样钳高(第 i 个顶点对应第 i/2 个折线点)。
    std::vector<float> lineClampSource;
    /// 贴地(ClampToGround + 后端支持 stencil 分类)时,fill/line 改产
    /// 挤出体走双 pass 像素级贴合;此时上面的 fill/lineVerts 为空(两条路
    /// **互斥**,同时产出会让同一份内容画两遍)。
    VolumeCpuGroups fillVolumeGroups;
    VolumeCpuGroups lineVolumeGroups;
    /// 点符号实例表(quad 定型留在渲染线程,见 TileSymbolCpu)。
    std::vector<TileSymbolCpu> symbols;

    bool empty() const {
        return fillIndices.empty() && lineIndices.empty() &&
               fillVolumeGroups.empty() && lineVolumeGroups.empty() &&
               symbols.empty();
    }
};

} // namespace earth_engine
