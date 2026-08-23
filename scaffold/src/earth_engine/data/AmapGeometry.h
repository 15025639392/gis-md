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
double amapCoordScale(int layerType, int layerZ);

/// tile-local → 经纬度(返回弧度;flipY=true 时 y 底朝上)。
Cartographic amapTileLocalToLngLat(int tileX, int tileY, int z,
                                   double localX, double localY,
                                   bool flipY = true);

/// 一个解码层 → 引擎 Feature 列表。
/// - type1/4:每个 ring 一条 LineString(properties["amap_class"]=classCode);
/// - type2/3:每个 ring 一个 Polygon(孔洞/环合并是后续细化)。
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
