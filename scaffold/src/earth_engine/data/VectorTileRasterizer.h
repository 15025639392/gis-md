#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "MvtDecoder.h"
#include "StyleFilter.h"

namespace earth_engine {

/// 矢量瓦片 → RGBA 位图(E4 贴地双轨制的第一段)。
///
/// **为什么栅格化而不是继续画几何**:贴地(terrain-conformal)的难点在于让
/// 矢量跟着地形起伏。三条路各有代价:
///   ① 逐顶点采地形高(P3 方案 A)—— 需要地形采样空间索引,且 LOD 切换要重钳;
///   ② stencil 分类(P6 方案 B)—— 像素级精确,但每个要素一个挤出体,底图
///      量级(万级)撑不住,且 Metal 侧未接线;
///   ③ **烘成纹理走影像通道**(本文件)—— 引擎已有成熟的 raster overlay →
///      地形合成管线(TerrainPageStore/上采样/LOD/mapping 全都现成),把矢量
///      变成一张影像喂进去,贴地**免费**。maplibre 的 RTT draping 是同一思路,
///      但它要自建 FBO 与纹理池;我们直接复用影像通道,连 RTT 都不必建。
/// 代价:栅格化后不再有矢量的分辨率无关性(放大会糊),故这是**底图大宗要素**
/// 的通道;编辑层/少量要素仍走几何路径(双轨制的另一轨)。
///
/// **坐标红利**:MVT 几何本来就是瓦片本地整数坐标(extent 默认 4096),
/// 瓦片即图像 —— 全程不需要投影/椭球/地理换算,px = x * size / extent 完事。
/// 这也是为什么这一段可以是纯函数:输入只有瓦片与样式,没有相机、没有地形。
///
/// 线程契约:纯函数、无全局状态 → **可在 worker 并发调用**。

/// 单个源图层的绘制参数。按 vector 顺序绘制,先画的在下。
struct VectorRasterLayerPaint {
    std::string layer;          ///< MVT 源图层名
    StyleFilter::Ptr filter;    ///< 逐要素过滤(空 = 全收),与 E2 同一套谓词
    int minZoom = 0;            ///< 该层生效的瓦片 zoom 闭区间
    int maxZoom = 24;
    /// RGBA(非预乘)。alpha=0 表示**不绘制该通道**(不是画透明)。
    std::array<uint8_t, 4> fillColor{0, 0, 0, 0};
    std::array<uint8_t, 4> lineColor{0, 0, 0, 0};
    /// 线宽,单位 = **输出图像像素**(不是米,也不是屏幕像素)。烘成纹理后
    /// 线宽随地形贴合被拉伸,与几何路径的"屏幕恒定宽"语义不同 —— 这是
    /// draping 的固有性质,maplibre 的 line 进 RTT 后同样如此。
    double lineWidthPixels = 1.0;
};

struct VectorRasterStyle {
    /// 底色。默认全透明 —— 底图矢量层通常叠在卫星影像上。
    std::array<uint8_t, 4> background{0, 0, 0, 0};
    std::vector<VectorRasterLayerPaint> layers;
    /// 抗锯齿超采样倍率(每轴)。2 = 4 个样本/像素,是质量与内存的常用折中;
    /// 1 = 关闭。>4 收益已不可辨而临时缓冲按平方增长。
    int supersample = 2;
};

/// 栅格化结果:RGBA8,行主序,size×size。
struct VectorRasterImage {
    int size = 0;
    std::vector<uint8_t> rgba;
    bool empty() const { return rgba.empty(); }
};

/// @param tile   已解码 MVT 瓦片
/// @param zoom   瓦片 zoom(供 filter 的 zoomCompare 与图层 zoom 区间)
/// @param size   输出边长(像素),典型 256/512
VectorRasterImage rasterizeMvtTile(const MvtTile& tile, int zoom,
                                   const VectorRasterStyle& style, int size);

} // namespace earth_engine
