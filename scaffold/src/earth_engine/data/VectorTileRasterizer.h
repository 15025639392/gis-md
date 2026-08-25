#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "Feature.h"
#include "MvtDecoder.h"
#include "VectorRasterStyle.h"

namespace earth_engine {

/// 矢量瓦片 → RGBA 位图(刀1「面 drape」的栅格化核心,E4 通路的复活改造版)。
///
/// **历史**:E4(2026-08-04)首建"栅格化冒充影像走地形合成"通路,2026-08-07
/// 因"页纹素封顶,近景/斜视下**线**糊成栅格块"整链删除。2026-08-13 架构定案
/// (矢量表示随负载)后按新分工复活:**面**(水/landuse/建筑色块)走本通路进
/// 页存储合成 —— 面对边缘模糊远不如线敏感,验收基线=影像级边缘;**线**(路网)
/// 不走这里,由 SDF 场+地形 FS 解算承接(刀2)。当年死因由分工规避,不复现。
///
/// **与 E4 版的关键差异 —— 任意目标矩形**:E4 版是"整瓦片=整图像"的固定映射,
/// 页存储把 fetchKey 钳到数据 maxZoom 后 scale-bias 放大,分辨率就此封顶。
/// 本版以 **unit Web-Mercator 目标矩形**为输出域,源瓦片按各自 key 仿射映射:
///   ① overzoom:z17 页 ← z14 矢量瓦的 1/64 子矩形**现画**,矢量分辨率无关,
///      清晰度与页 zoom 同步(这正是"动态栅格化"的含义);
///   ② 跨瓦拼接:目标矩形跨多张源瓦(GCJ 平移后的常态)一次画进同一画布;
///   ③ GCJ-02 对齐:页网格按首源(高德)建,调用方把矩形逆平移回 WGS84 后
///      喂进来,本函数不感知投影 —— 仍是纯函数。
///
/// 算法沿用 E4 验证过的三件套:nonzero 环绕数扫描线填充(MVT 孔环反绕向天然
/// 挖孔;even-odd 在 OSM 自相交多边形上挖错洞)、折线方形接头描边(miter 甩
/// 长刺)、超采样盒式降采样 AA。新增 per-path 画布外 bbox 剔除:overzoom 下
/// 源瓦 63/64 在画布外,不剔则每页都全量扫全瓦要素。bbox 与画布不相交 ⇒ 环
/// 既不进入也不可能包围画布(包围 ⇒ bbox ⊇ 画布 ⇒ 相交),跳过恒安全。
///
/// 线程契约:纯函数、无全局状态 → 可在 worker 并发调用。

/// 栅格化结果:RGBA8,行主序,size×size。
struct VectorRasterImage {
    int size = 0;
    std::vector<uint8_t> rgba;
    bool empty() const { return rgba.empty(); }
};

/// 参与栅格化的一张源瓦片:已解码 MVT + 它的 XYZ Web-Mercator 瓦片坐标。
struct MvtTileRef {
    const MvtTile* tile = nullptr;
    int z = 0;
    int x = 0;
    int y = 0;
};

/// unit Web-Mercator 矩形([0,1]²;x 东向,y 北→南,与 XYZ 瓦片行序同向)。
/// 瓦片 (z,x,y) 的范围即 [x,x+1]/2^z × [y,y+1]/2^z。
struct MercatorRect {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 1.0;
    double y1 = 1.0;
};

/// 逐顶点坐标变换:把 OSM 顶点的 WGS84 unit-mercator 坐标(u,v)**原地**改写成
/// 目标采样空间的 unit 坐标。GCJ-02 底图传 wgs→gcj(fromWgs84),使矢量内容与
/// 地形逐顶点 GCJ texcoord(TileRasterOverlayDetailsGenerator 逐顶点 fromWgs84)
/// **同精度**对齐 —— 整页单点平移在大页/overzoom 祖先页边缘发散上百米,是真机
/// 中景错位的根因。nullptr = 恒等(标准 overlay),逐顶点走线性映射,视觉等价
/// 改造前。
using UnitTransform = std::function<void(double& u, double& v)>;

/// 把若干源瓦片中落进 rect 的要素画成 size×size RGBA 图。
/// @param styleZoom 样式求值 zoom(层 min/maxZoom 区间与 filter 的
///        zoomCompare)。用**页 zoom**而非源瓦 z:building 只在近景之类的
///        门槛应跟屏幕清晰度走,而源瓦 z 被数据 maxZoom 钳死。
/// @param toTargetUnit 逐顶点变换(见 UnitTransform);GCJ 底图必传,否则大页
///        错位。rect 是**目标采样空间**矩形(GCJ 时=GCJ 页矩形)。
VectorRasterImage rasterizeMvtRect(const std::vector<MvtTileRef>& tiles,
                                   const MercatorRect& rect, int styleZoom,
                                   const VectorRasterStyle& style, int size,
                                   const UnitTransform* toTargetUnit = nullptr);

/// E4 兼容便捷形:单瓦整图(rect=整张瓦、styleZoom=zoom)。测试与调试用。
VectorRasterImage rasterizeMvtTile(const MvtTile& tile, int zoom,
                                   const VectorRasterStyle& style, int size);

/// 把已是经纬度(弧度)的 Polygon Feature 画进 mercator 目标矩形。
///
/// Amap type2 drape 入口:解码侧已完成 tile-local 裁剪 / even-odd 归一化 /
/// GCJ→WGS84,本函数只做球面→unit mercator→画布,扫描线与
/// `rasterizeMvtRect` 共用。`paint.layer` 对 Amap 匹配 `amap_fillkey`
/// (空或 `"*"` = 全收,再叠加 `paint.filter`)。`toTargetUnit` 与
/// `rasterizeMvtRect` 同语义(GCJ 页网格传 `wgsUnitToGcjUnit`)。
VectorRasterImage rasterizeFeaturePolygonsRect(
    const std::vector<const Feature*>& features, const MercatorRect& rect,
    int styleZoom, const VectorRasterStyle& style, int size,
    const UnitTransform* toTargetUnit = nullptr);

} // namespace earth_engine
