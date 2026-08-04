#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "MvtDecoder.h"
#include "VectorTileRasterizer.h"  // VectorRasterStyle(两条路径共用一套样式)

namespace earth_engine {

/// 矢量瓦片 → 瓦片本地 2D 三角网(C-2b,页存储 GPU 贴地路径的几何来源)。
///
/// **为什么不是继续栅格化**:E4-1 的 `rasterizeMvtTile` 把瓦片烘成固定分辨率的
/// 位图,页 zoom 深于源 zoom 时只能放大 —— 真机实测 z14 源贴到 z17 页要放大 8 倍,
/// 路网糊成灰带。GPU 路径把镶嵌结果留在**瓦片本地归一化坐标**里,页只是换一个
/// 正交矩阵的子矩形 → **一份网格服务所有页 zoom,overzoom 免费**。
///
/// **线宽走顶点着色器挤压**(maplibre line shader 同法):`extrude` 存的是
/// 「单位法线 × 半线宽(页像素)」,顶点着色器按「页像素 → 瓦片归一化」的比例
/// 展开。若把线宽烘进顶点位置,网格就绑死在某一个页 zoom 上,overzoom 红利立刻
/// 消失 —— 这是本设计的支点,勿改。
///
/// 线程契约:纯函数、无全局状态 → **可在 worker 并发调用**(同 rasterizeMvtTile)。
///
/// 已知取舍:线段四边形与接头方块**重叠**。不透明线色下不可见(GPU 混合下后画
/// 的覆盖先画的);半透明线色会在接头处叠深。E4-1 的栅格路径靠 nonzero 覆盖 mask
/// 规避,GPU 侧要规避需 stencil 或单遍 join 镶嵌,底图线宽下不值。

/// 20B 顶点。布局与 GLES/Metal 侧的 VectorPageMesh20 属性表一一对应。
struct VectorTileMeshVertex {
    /// 瓦片本地**归一化**坐标 [0,1](按各图层自己的 extent 归一 —— extent 是
    /// per-layer 的,不能全局假设 4096)。y 向下,与 MVT 原始约定一致。
    float x = 0.0f;
    float y = 0.0f;
    /// 挤压向量 = 单位法线 × 半线宽(单位:**页像素**)。面恒 (0,0)。
    float ex = 0.0f;
    float ey = 0.0f;
    uint8_t r = 0, g = 0, b = 0, a = 0;  ///< 非预乘 RGBA
};

static_assert(sizeof(VectorTileMeshVertex) == 20,
              "VectorPageMesh20 顶点布局契约:后端属性偏移按 20B 写死");

struct VectorTileMesh {
    std::vector<VectorTileMeshVertex> vertices;
    /// 三角形索引,**已按样式层序 + 层内先面后线排好 = 画家序**。该 pass 不开
    /// 深度测试,靠绘制顺序定压盖关系,故索引顺序即最终视觉顺序。
    std::vector<uint32_t> indices;

    bool empty() const { return indices.empty(); }
};

/// @param tile  已解码 MVT 瓦片
/// @param zoom  **源瓦片自己的 zoom**(供 filter 的 zoomCompare 与图层 zoom
///              区间求值)。注意不是页 zoom —— 页可能比源深,样式分级要按源算,
///              否则同一份网格在不同页上会要求不同的要素集合,网格就不能共享了。
VectorTileMesh buildVectorTileMesh(const MvtTile& tile, int zoom,
                                   const VectorRasterStyle& style);

}  // namespace earth_engine
