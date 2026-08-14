#pragma once

#include <cstdint>
#include <vector>

#include "MvtDecoder.h"
#include "VectorRasterStyle.h"
#include "VectorTileRasterizer.h"  // MvtTileRef / MercatorRect / UnitTransform

namespace earth_engine {

/// 路网线「线段纹素」场烘焙(D2,场线宽像素一致专项定稿编码)。
///
/// **为什么不是距离场**:标量/向量距离场都靠双线性插值重建,插值本身就是
/// 伪影之源(标量跨线心尖点必高估 → 漏画;向量跨双线中轴必过零 → 幽灵;
/// 真实路网实测标量漏画 63%)。D2 每纹素存**最近线段的局部线段参数**,
/// FS 取 2×2 邻域 4 条线段各自解析算胶囊距离取 min ——**全程无插值**。
/// MVT 路网是折线、段内即直线 → 局部线性模型精确;端点余量让胶囊在真实
/// 端点收口,拐角两胶囊圆帽相接 = 天然圆角。**重建 = 精确,仅剩量化误差**
/// (真实路网模拟:texelPx=4 下漏画 0.28%、有害幽灵 0、误差 0.025px)。
///
/// **RGBA8 编码(每纹素一条线段)**:
///   R,G = 最近点偏移 (ox,oy),范围 ±kLineFieldOffsetRangeTexels,
///         各 8bit。⚠️ 保持双分量冗余:偏移⊥方向,理论可压成"有符号距离
///         +角度"省 1 字节,但模拟证伪(误差 0.025→0.048px、幽灵回潮)——
///         θ 量化误差会旋转锚点,(ox,oy) 的冗余实为误差解耦,勿"优化"。
///   B   = 线段方向角 θ ∈ [0,π),8bit。方向经规范化(uy>0 或 uy==0&&ux>0),
///         fwd/back 在规范化方向下定义。
///   A   = fwd(高 4bit)| back(低 4bit):最近点到线段两端的剩余长度,
///         各 0..kLineFieldClampMaxTexels 按 kLineFieldClampStepTexels 量化。
///         **A==0 是空哨兵**(该纹素 kOffsetRange 内无线);合法编码若恰为 0
///         (退化微段)提升为 1。全 0 纹素 = 失败安全(未绑定采样/memset 0
///         都表现为"无线")。FS 靠"像素可被线覆盖 ⇒ 所在纹素必有记录"做
///         单 fetch 早退(线覆盖半径 + √2/2 < 编码范围)。
///
/// **烘焙算法:逐段 scatter**(与旧编码同构):每段写自己膨胀 bbox 内纹素的
/// min-dist 记录(距离胜者的段参数整套落纹素)。复杂度不变,亚毫秒/页。
///
/// 坐标/矩形语义与 rasterizeMvtRect 同构(unit-mercator 目标矩形 + 多源瓦
/// 仿射,overzoom 子矩形现画、GCJ 逐顶点变换由调用方给)。样式只消费 line
/// 通道(lineColor.a>0 的层做 zoom/filter 闸;lineWidthPixels 不消费——宽度
/// 语义整体在 FS uniform 侧)。纯函数,worker 可并发。
struct LineFieldImage {
    int size = 0;
    std::vector<uint8_t> rgba8;  // 行主序,4B/texel;全 0 = 无线(失败安全)
    bool empty() const { return rgba8.empty(); }
};

/// 偏移编码范围(texel)。上限约束:线覆盖半径(最大线半宽 px→texel)+
/// 像素到纹素中心 √2/2 必须 ≤ 此值,否则宽线边缘像素的所在纹素无记录、
/// 被哨兵早退误杀(d=0 时 ramp 顶 3.15px = 3.15 texel → 3.86 < 4 ✓)。
/// 量化步进 = 2×4/255 ≈ 0.031 texel。
constexpr double kLineFieldOffsetRangeTexels = 4.0;
/// 端点余量上限(texel)与量化步进(4bit,15 级)。下限约束:相邻纹素记录
/// 点沿线间距 ≤ √2,胶囊半长 1.5 保证拼接无缝。
constexpr double kLineFieldClampMaxTexels = 1.5;
constexpr double kLineFieldClampStepTexels = 0.1;

/// @param toTargetUnit 逐顶点变换(GCJ 底图必传 fromWgs84);nullptr = 线性。
LineFieldImage rasterizeLineFieldRect(const std::vector<MvtTileRef>& tiles,
                                      const MercatorRect& rect, int styleZoom,
                                      const VectorRasterStyle& style, int size,
                                      const UnitTransform* toTargetUnit =
                                          nullptr);

} // namespace earth_engine
