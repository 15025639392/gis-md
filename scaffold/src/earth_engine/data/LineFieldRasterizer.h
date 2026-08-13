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
/// **编码:到线*边缘*的有符号距离(半宽烘进场)**。单一"到中心线距离"场
/// 表达不了 highway 分档线宽(motorway 与步道不同宽);烘焙期每段按自己
/// 档位的半宽膨胀,场值 = distToCenterline − halfWidth(texel),多段取
/// min —— FS 端只做统一的 0 交叉 smoothstep,任意每段宽免费。
/// 量化(**反向**):value = clamp(0.5 − sd/(2·kFeatherTexels), 0, 1),
/// 0.5=线边缘,>0.5 在线内,**0=远离一切线** —— 0 是失败安全值:GLES 未
/// 绑定 sampler 采样返回 0、占位清场 memset 0、任何垃圾 0 都表现为"无线"
/// 而不是"全屏线色"。max(编码值) 与 encode(min(sd)) 等价(编码反单调)。
///
/// **烘焙算法:逐段 scatter,不是逐 texel gather**。每段只写自己
/// (halfWidth+kFeatherTexels) 膨胀 bbox 内的 texel(点到线段距离取 min)。
/// 复杂度 O(段数 × 段bbox纹素),z14 城区瓦几百段 × ~百 texel ≈ 亚毫秒/页;
/// PoC 的"2048² 全场暴力"印象不适用于此结构。
///
/// 坐标/矩形语义与 rasterizeMvtRect 完全同构(unit Web-Mercator 目标矩形 +
/// 多源瓦仿射,overzoom 子矩形现画、跨瓦拼接、GCJ 平移由调用方做);线宽
/// 单位=输出纹素 ≈ 设备像素(页按屏幕误差选 zoom,页纹素≈屏幕像素)。
///
/// 样式:复用 VectorRasterStyle,但**只消费 line 通道**(lineColor.a>0 的
/// 层;fillColor 忽略)—— 线色不进场(场只有覆盖/距离),颜色在 FS 端以
/// uniform 统一给出,首版单色。
///
/// 线程契约:纯函数、无全局状态 → worker 可并发。
struct LineFieldImage {
    int size = 0;
    std::vector<uint8_t> r8;  // 行主序;0=无线,>128 在线内
    bool empty() const { return r8.empty(); }
};

/// 羽化半带宽(texel):编码窗口 = 边缘 ±kFeatherTexels。窄了 FS 的
/// fwidth AA 无料可插,宽了量化步进变粗(2·4/255 ≈ 0.03 texel/级,够细)。
constexpr double kLineFieldFeatherTexels = 4.0;

/// @param toTargetUnit 逐顶点变换(见 VectorTileRasterizer.h 的 UnitTransform);
///        GCJ 底图必传(fromWgs84),否则大页/祖先页边缘错位。rect 是目标采样
///        空间矩形。nullptr = 标准 overlay 线性映射。
LineFieldImage rasterizeLineFieldRect(const std::vector<MvtTileRef>& tiles,
                                      const MercatorRect& rect, int styleZoom,
                                      const VectorRasterStyle& style, int size,
                                      const UnitTransform* toTargetUnit =
                                          nullptr);

} // namespace earth_engine
