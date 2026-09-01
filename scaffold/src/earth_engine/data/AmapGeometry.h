#pragma once

#include "AmapVectorTile.h"
#include "Feature.h"

#include <string>
#include <vector>

namespace earth_engine {

#if defined(EARTH_ENGINE_TESTING)
struct AmapDecodedTile {
    std::vector<AmapDecodedLayerPart> parts;
};

struct AmapDecodedTileDecodeTraits {
    static bool decode(const uint8_t* data, size_t size,
                       AmapDecodedTile& out, std::string* error);
    static size_t approxBytes(const AmapDecodedTile& tile);
};
#endif

/// 高德瓦片几何 → 引擎 Feature(lon/lat 弧度,WGS84)。
///
/// 高德 4326 等距圆柱瓦片:2:1 地理比例(经度 360/2^z、纬度 180/2^z),
/// 规范 tile-local 空间 = 8192(x) × 4096(y),y 自北向南(top-down,
/// 参考 amap_geometry.js/amap_reproject.js 实测结论)。各层 type 的原始
/// extent 不同,解码器产出原始整数坐标,这里按层 type/zoom 取 coordScale
/// 抬进规范空间:
///   type0/type4 point 标签:使用 PointFeatureSameStyle.resolution 的显式
///                           2^(14-resolution) 倍率;
///   type1 线 / type4 轨道:z14+ → scale 2(原始 4096×2048),
///                          z6-12 → 4,z3 → 8;
///   type2/type4 区域:默认 scale 4(原始 2048×1024),kind 60/64/80
///                     改走对应 zoom 的 line-grid scale;
///   type3 建筑:由 BuildingSameStyle.resolution 决定。
/// 转换后可选 GCJ-02 → WGS84 反偏移(引擎已有 Gcj02CoordinateTransform)。
/// 层类型 → tile-local 原始整数坐标抬进规范 8192×4096 空间的倍率。
/// type0 POI 标签:z6-14 → 4(2048×1024),z3 → 8(1024×512);
/// type1 线 / type4 轨道:z14+ → 2(4096×2048),z6-12 → 4,z3 → 8;
/// type2 区域:默认恒 4(2048×1024,任意 zoom),**大区域 kind 60/80
/// (type4 的 kind 64)例外** —— 它们走 line-grid(同 type1,见
/// xinzhi-map decodeRegionFeature LINE_GRID_REGION_KINDS);
/// type3 建筑倍率 = 2^(14-resolution)。官方默认 resolution=12 对应
/// 2048×1024、倍率 4；当前真实 payload resolution=18 对应
/// 131072×65536、倍率 1/16。
double amapCoordScale(int layerType, int layerZ, int regionKind = 0,
                      int buildingResolution = 12);

/// Validates the official runtime's
/// `getCoordShift(z, resolution) = 33 - resolution - z` contract.
/// Invalid provider values fail closed instead of wrapping a JS bit shift or
/// selecting a local fallback grid.
bool amapBuildingResolutionIsValid(int layerZ, int buildingResolution);

/// 大区域 kind 是否走 line-grid(而非 type2 恒定的 2048×1024 网格)。
/// 参考 xinzhi-map LINE_GRID_REGION_KINDS = {60, 64, 80}。
bool amapRegionUsesLineGrid(int regionKind);

/// canonical tile-local → 经纬度(返回弧度)。canonical Y 与高德参考实现
/// `loadGeometry()` 的输出一致：y=0 在瓦片北缘、y=4096 在南缘。
Cartographic amapTileLocalToLngLat(int tileX, int tileY, int z,
                                   double localX, double localY);

/// 高德区域环归一化(参考 xinzhi-map amap_geometry.js
/// `normalizeEvenOddWinding`)。
///
/// 高德 type2 区域(type3 建筑同)的环**全部同向绕行**,按 **even-odd** 规则
/// 填充(海洋掩膜嵌套陆地、湖泊嵌套岛屿、建筑天井嵌套)。Mapbox/本引擎的
/// fill 走 **nonzero** 规则(外环 CCW、孔 CW),直接喂同向环会把被包围的
/// 陆地/岛屿画成水面、天井画成实心。
///
/// 算法:对每对环做「内点 + 射线法」求 even-odd 嵌套深度;偶数深度 = 外环
/// (重绕为正向),奇数深度 = 孔(重绕为负向,挂在最近包围它的外环下)。
/// 输入/输出都是 **canonical top-down tile-local 平面坐标**
/// (面积/绕向/点在环内在平面才稳定)，调用方先完成 scale + raw Y 翻转。
///
/// @param rings 同一 feature 的全部环(tile-local 整数坐标)。
/// @return 重排/重绕后的环列表:每个外环紧跟着它的孔环。
std::vector<std::vector<std::pair<double, double>>> amapNormalizeEvenOddWinding(
    const std::vector<std::vector<std::pair<double, double>>>& rings);

/// 高德瓦片多边形裁剪(参考 xinzhi-map amap_reprojected_tile.js
/// `clipPolygonRing`,Sutherland–Hodgman 对 4 个轴对齐半平面)。
///
/// geometry blob 的 polygon ring 可省略重复首点，但语义上仍是闭合面；
/// 裁剪包含隐式 last→first 边，结果也由消费端按 modulo 隐式闭合。
/// 调用方应传入翻转后的 canonical tile-local 坐标(与
/// amapNormalizeEvenOddWinding 同一坐标系)。
///
/// @param ring 一个环(tile-local 坐标,可越界、可省略重复首点)。
/// @param minX/maxX/minY/maxY 裁剪窗口(x/y 独立,参考 POLY_CLIP_BUFFER=
///   256:min=-256, max=extent+256)。返回空 = 无幸存。
std::vector<std::pair<double, double>> amapClipPolygonRing(
    const std::vector<std::pair<double, double>>& ring,
    double minX, double maxX, double minY, double maxY);

/// 一个解码层 → 引擎 Feature 列表。
/// - type0 与 type4 content.#2:每个 point 输出一个 Point;
/// - type1 与 type4 content.#1:每个 ring 一条 LineString;
/// - type2、type3 与 type4 content.#3:区域环经 winding 归一化后输出
///   Polygon；type4 的 line/area 由 feature 显式语义区分。
/// toWgs84=true 时做 GCJ 反偏移(默认)。
#if defined(EARTH_ENGINE_TESTING)
std::vector<Feature> amapDecodedPartToFeatures(
    const AmapDecodedLayerPart& part, bool toWgs84 = true);
#endif

}  // namespace earth_engine
