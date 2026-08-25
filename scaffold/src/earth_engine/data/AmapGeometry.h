#pragma once

#include "AmapVectorTile.h"
#include "Feature.h"

#include <string>
#include <vector>

namespace earth_engine {

/// 高德瓦片几何 → 引擎 Feature(lon/lat 弧度,WGS84)。
///
/// 高德 4326 等距圆柱瓦片:2:1 地理比例(经度 360/2^z、纬度 180/2^z),
/// 规范 tile-local 空间 = 8192(x) × 4096(y),y 底朝上(flipY=true,
/// 参考 amap_geometry.js/amap_reproject.js 实测结论)。各层 type 的原始
/// extent 不同,解码器产出原始整数坐标,这里按层 type/zoom 取 coordScale
/// 抬进规范空间:
///   type1 线 / type4 轨道:z14+ → scale 2(原始 4096×2048),
///                          z6-12 → 4,z3 → 8;
///   type2 区域:恒 scale 4(原始 2048×1024,kind 60/80 大区域除外,后续);
///   type3 建筑:scale 1/16(原始 131072×65536)。
/// 转换后可选 GCJ-02 → WGS84 反偏移(引擎已有 Gcj02CoordinateTransform)。
/// 层类型 → tile-local 原始整数坐标抬进规范 8192×4096 空间的倍率。
/// type1 线 / type4 轨道:z14+ → 2(4096×2048),z6-12 → 4,z3 → 8;
/// type2 区域:默认恒 4(2048×1024,任意 zoom),**大区域 kind 60/80
/// (type4 的 kind 64)例外** —— 它们走 line-grid(同 type1,见
/// xinzhi-map decodeRegionFeature LINE_GRID_REGION_KINDS);
/// type3 建筑:1/16(131072×65536)。
double amapCoordScale(int layerType, int layerZ, int regionKind = 0);

/// 大区域 kind 是否走 line-grid(而非 type2 恒定的 2048×1024 网格)。
/// 参考 xinzhi-map LINE_GRID_REGION_KINDS = {60, 64, 80}。
bool amapRegionUsesLineGrid(int regionKind);

/// tile-local → 经纬度(返回弧度;flipY=true 时 y 底朝上)。
Cartographic amapTileLocalToLngLat(int tileX, int tileY, int z,
                                   double localX, double localY,
                                   bool flipY = true);

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
/// 输入/输出都是 **tile-local 平面坐标**(面积/绕向/点在环内在平面才稳定),
/// 调用方随后再乘 coordScale 转经纬度。
///
/// @param rings 同一 feature 的全部环(tile-local 整数坐标)。
/// @return 重排/重绕后的环列表:每个外环紧跟着它的孔环。
std::vector<std::vector<std::pair<double, double>>> amapNormalizeEvenOddWinding(
    const std::vector<std::vector<std::pair<double, double>>>& rings);

/// 高德瓦片多边形裁剪(参考 xinzhi-map amap_reprojected_tile.js
/// `clipPolygonRing`,Sutherland–Hodgman 对 4 个轴对齐半平面)。
///
/// 高德 4326 瓦片的面环**跨瓦片边界时被裁成开放条带**(首尾不连,且
/// 部分坐标越界)。不裁剪直接补闭合会得到贴瓦片边缘的细长伪多边形
/// (蓝色观感异常的根因)。裁剪把环切到 [min, max]² 窗口内,开口沿瓦片
/// 边闭合,再交给三角化才得到正确面。调用方应传入**翻转后 canonical
/// tile-local 坐标**(与 amapNormalizeEvenOddWinding 同一坐标系)。
///
/// @param ring 一个环(tile-local 坐标,可越界/开放)。
/// @param minX/maxX/minY/maxY 裁剪窗口(x/y 独立,参考 POLY_CLIP_BUFFER=
///   256:min=-256, max=extent+256)。返回空 = 无幸存。
std::vector<std::pair<double, double>> amapClipPolygonRing(
    const std::vector<std::pair<double, double>>& ring,
    double minX, double maxX, double minY, double maxY);

/// 裁剪后的开放环**沿裁剪窗口闭合**(不是直线)。
///
/// Sutherland–Hodgman 裁剪把跨窗口环切成一段开放弧:首尾落在窗口边界上,
/// 数据语义里真正补完它的边是**窗口边界的路径**(参考高德 JSAPI
/// clipPolygon 的 polygonclip 语义)。若首尾在窗口边界上,这里沿边界补
/// 路径(两条候选路径选与环自身绕向同号者 —— 归一化后外环 area>0、
/// 孔 area<0,闭合不得翻转绕向);首尾在窗口内 = 完全在窗口内的小水体
/// (水塘/河流段),直线补首点。region blob 头已正确跳过(见
/// AmapVectorTile.cpp),环是真实边界,不再需要 ratio 退化过滤 ——
/// 旧过滤为「头部误当首点」产生的假碎片而设,会误杀细长河流段。
///
/// @param ring 裁剪后的开放环(canonical tile-local 坐标,可空)。
/// @return 闭合后的环;不足 3 点返回空。
std::vector<std::pair<double, double>> amapCloseRingAlongWindow(
    std::vector<std::pair<double, double>> ring,
    double winMinX, double winMaxX, double winMinY, double winMaxY);

/// 一个解码层 → 引擎 Feature 列表。
/// - type1/4:每个 ring 一条 LineString(properties["amap_class"]=classCode);
/// - type2/3:环先经 amapNormalizeEvenOddWinding 归一化,每个「外环+孔环」
///   组合成一个 Polygon(rings[0]=外环,rings[1..]=孔,三角化自动挖孔)。
/// toWgs84=true 时做 GCJ 反偏移(默认)。
std::vector<Feature> amapDecodedPartToFeatures(
    const AmapDecodedLayerPart& part, bool toWgs84 = true);

/// 完整瓦片字节流 → 引擎 Feature 列表(解码 + 逐层转换)。
///
/// E3 通路:高德瓦片容器(4 字节 BE 长度 + gzip protobuf)直接解码成
/// Feature,供 VectorTileSourceT 的 DecodeTraits 注入。regionsOnly 过滤
/// 在解码阶段做(粗源 z10 只要 type2 面,主源 z14 只要 type1/3/4)——
/// 与旧 demo 切片 amapLoadDemoTile 的过滤语义一致,但提前到 worker 解码
/// 期,减少传输/缓存里的无关要素。
bool amapBytesToFeatures(const uint8_t* data, size_t size,
                         bool regionsOnly, std::vector<Feature>& out,
                         std::string* error = nullptr);

}  // namespace earth_engine
