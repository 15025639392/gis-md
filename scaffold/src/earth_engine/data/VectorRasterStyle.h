#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "MvtDecoder.h"
#include "StyleFilter.h"

namespace earth_engine {

/// 矢量底图的绘制样式(颜色/线宽/图层过滤/超采样)。
///
/// 曾经服务两条通路,现在只剩几何通路(FeatureRenderLayer + 瓦片桶)。
/// 对立的影像通路——把瓦片烘成 RGBA 位图冒充 raster overlay 走地形合成
/// (`rasterizeMvtTile` / `VectorImageryProvider` / `VectorPageDrawer`)——
/// 已于 2026-08-07 整链删除:页纹素给分辨率封了顶,近景/斜视下线糊成栅格块。
///
/// 纯数据结构,无状态 → worker 可并发按值读取。

/// 单个源图层的绘制参数。按 vector 顺序绘制,先画的在下。
struct VectorRasterLayerPaint {
    std::string layer;          ///< MVT 源图层名
    StyleFilter::Ptr filter;    ///< 逐要素过滤(空 = 全收),与 E2 同一套谓词
    int minZoom = 0;            ///< 该层生效的瓦片 zoom 闭区间
    int maxZoom = 24;
    /// RGBA(非预乘)。alpha=0 表示**不绘制该通道**(不是画透明)。
    std::array<uint8_t, 4> fillColor{0, 0, 0, 0};
    std::array<uint8_t, 4> lineColor{0, 0, 0, 0};
    /// 线宽,单位 = 输出像素。几何通路把它当作屏幕空间半宽喂进顶点挤压
    /// (见 VectorTileMeshBuilder 的 extrude)。
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

} // namespace earth_engine
