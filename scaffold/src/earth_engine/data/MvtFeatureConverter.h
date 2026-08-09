#pragma once

#include "Feature.h"
#include "MvtDecoder.h"
#include "../core/math/Rectangle.h"
#include "../tiling/TileKey.h"

#include <vector>

namespace earth_engine {

/// MVT 瓦片本地坐标 → cartographic(radian,height=0)。
///
/// XYZ WebMercator 数学:经度对瓦片 x 线性;纬度走反 mercator
/// (瓦片本地 y 线性于 mercator y,不线性于纬度——不能用
/// tileToRectangle 的矩形线性插值,那会在瓦片内部产生纬向漂移)。
/// 坐标允许越界(buffer 区)与负值,照常外推。
Cartographic mvtToCartographic(const TileKey& key, uint32_t extent,
                               const MvtPoint& p);

/// 一层 MVT 要素 → 引擎 Feature 列表(共享下游渲染/拾取/snap)。
///
/// - Multi* 拆成多个 Feature(Feature 的 rings 语义是单几何)
/// - Polygon 环经 classifyMvtRings 分外环/孔
/// - 退化几何丢弃(线 <2 点)
/// - properties 逐 Feature 复制,并写入 properties["mvt_layer"] =
///   layer.name(样式表达式按源图层分流用,同 osgearth 约定)
/// - sourceId = MVT feature id(0 = 未携带,置空);Feature::id 不填,
///   由目标 FeatureStore 分配
std::vector<Feature> mvtLayerToFeatures(const MvtLayer& layer,
                                        const TileKey& key);

/// 该 MVT 瓦片覆盖的地理矩形(弧度)。与 mvtToCartographic 同一套 XYZ↔经纬
/// 换算 —— 复制一份公式的代价是它会随投影约定悄悄漂移,而漂移的表现是「贴地
/// 高度范围取错了邻块」,只在特定瓦片边界上现形。
Rectangle mvtTileRectangle(const TileKey& key);

} // namespace earth_engine
