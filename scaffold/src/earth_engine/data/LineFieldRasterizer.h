#pragma once

#include <cstdint>
#include <vector>

#include "MvtDecoder.h"
#include "VectorRasterStyle.h"
#include "VectorTileRasterizer.h"  // MvtTileRef / MercatorRect

namespace earth_engine {

/// 路网线 SDF 场烘焙(刀2「线 SDF 场 + 地形 FS 解算」的 CPU 端)。
///
/// **为什么是场而不是几何/位图**:线对栅格模糊极敏感(E4 死因),直接把线
/// 画进位图会在放大时糊成栅格块;SDF 场的边缘由**场的梯度**在片元里重建
/// (smoothstep + fwidth 解析 AA),锐度与纹素密度解耦 —— S3 PoC 实测
/// 4.4m/texel 下近场+掠视全程平滑无块状。
///
/// **编码:归一化中心线距离(宽度不进场)**。
/// value = clamp(1 − distToCenterline/kLineFieldBandTexels, 0, 1),多段取
/// min(dist) = max(编码值)(编码反单调)。1=线心,**0=远离一切线** ——
/// 0 是失败安全值:GLES 未绑定 sampler 采样返回 0、占位清场 memset 0、
/// 任何垃圾 0 都表现为"无线"而不是"全屏线色"。
/// 宽度**不再烘进场**:FS 端用采样 UV 的屏幕导数求 texel/px 比,
///   distPx = (1 − fieldV) · kLineFieldBandTexels / texPerPx
/// 再按"线半宽(设备px)"阈值 + AA —— 页内近端放大/祖先页兜底/页界跳档
/// 全被导数自动补偿,线宽真屏幕像素恒定(场线宽像素一致专项,
/// 2026-08-14;分母取几何导数而非 fwidth(fieldV) 的原因见
/// PageStoreSamplingGLSL.h)。分档宽度语义在 FS uniform 侧表达,不在场里。
///
/// **烘焙算法:逐段 scatter,不是逐 texel gather**。每段只写自己
/// kLineFieldBandTexels 膨胀 bbox 内的 texel(点到线段距离取 min)。
/// 复杂度 O(段数 × 段bbox纹素),z14 城区瓦几百段 × ~2 百 texel ≈ 亚毫秒/页;
/// PoC 的"2048² 全场暴力"印象不适用于此结构。
///
/// 坐标/矩形语义与 rasterizeMvtRect 完全同构(unit Web-Mercator 目标矩形 +
/// 多源瓦仿射,overzoom 子矩形现画、跨瓦拼接、GCJ 平移由调用方做)。
///
/// 样式:复用 VectorRasterStyle,但**只消费 line 通道**(lineColor.a>0 的
/// 层;fillColor 忽略,lineWidthPixels 不再消费——宽度归 FS uniform)——
/// 线色不进场(场只有距离),颜色在 FS 端以 uniform 统一给出,首版单色。
///
/// 线程契约:纯函数、无全局状态 → worker 可并发。
struct LineFieldImage {
    int size = 0;
    std::vector<uint8_t> r8;  // 行主序;0=无线,255=线心
    bool empty() const { return r8.empty(); }
};

/// 编码带宽(texel):中心线 0 → band 处衰减到 0。上限:须 ≥ 最大线半宽
/// (设备px)×(texel/px 最小放大比),否则宽线被 clamp 截断;下限:R8
/// 量化步进 = band/255 texel/级,会被 FS 的 ÷fwidth 放大(8 倍放大页下
/// 8/255 ≈ 0.03 texel/级 → ~0.25px 抖动,PoC 真机验收此项)。
constexpr double kLineFieldBandTexels = 8.0;

/// @param toTargetUnit 逐顶点变换(见 VectorTileRasterizer.h 的 UnitTransform);
///        GCJ 底图必传(fromWgs84),否则大页/祖先页边缘错位。rect 是目标采样
///        空间矩形。nullptr = 标准 overlay 线性映射。
LineFieldImage rasterizeLineFieldRect(const std::vector<MvtTileRef>& tiles,
                                      const MercatorRect& rect, int styleZoom,
                                      const VectorRasterStyle& style, int size,
                                      const UnitTransform* toTargetUnit =
                                          nullptr);

} // namespace earth_engine
