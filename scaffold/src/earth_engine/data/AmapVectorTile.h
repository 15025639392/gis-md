#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace earth_engine {

/// 高德(Nebula)矢量瓦片解码(参考 xinzhi-map
/// packages/engine/src/custom/amap/amap_vector_tile.js 的已验证 schema,
/// 按当前数据版本 26_07_27_00 校准)。
///
/// 容器: [4 字节大端长度][gzip(protobuf)];长度 = 尾随 gzip 流字节数。
/// root   { Tile #1, version #2(string,忽略) }
/// Tile   { z #1, x #2, y #3, repeated Layer #4 }          —— 容器
/// Layer  { z #1, x #2, y #3, type #4, content #5, #8 }
///   type 1 = 线(道路/轨道), type 2 = 面(区域/水/绿地),
///   type 3 = 建筑(footprint + height,仅 z14 容器内以 z15 子层出现),
///   type 4 = 轨道线;POI 组(type=2 请求)另有 0/1/4 标签类。
/// content { repeated ClassGroup(实测字段 1) }
/// ClassGroup { classCode #1(实测;参考文档亦可 #2), geomType #3(实测;参考 #2),
///              repeated Feature #4 }
/// Feature  { ..., repeated Part #4 }
/// Part     { blob #3 = 几何(全 zigzag 增量), height #5(参考 varint;实测
///            当前版本为 bytes,待校准——见 decodeAmapTile 注释) }
///
/// 几何 blob:首点也 zigzag 编码,随后 zigzag(dx,dy) 增量;产出瓦片局部
/// 坐标(整数),extent/coordScale 按 layer type 不同,由调用方换算经纬度
/// (参考 amap_geometry.js / amap_reproject.js)。
struct AmapDecodedFeature {
    int classCode = 0;
    int geomType = 0;
    /// 类组字段 2:type2 区域 = kind(水63/绿地61/建筑块20-27…),
    /// type3 建筑 = cat。样式配色按它分。
    int kind = 0;
    /// 环/折线:rings[0] = 一条;建筑每个 Part 一个环。坐标 = 瓦片局部整数。
    std::vector<std::vector<std::pair<double, double>>> rings;
    /// 建筑高度(米;type 3)。当前版本编码待校准,解析失败保持 0。
    double height = 0.0;
    /// 名称(道路名/POI 标签;Feature 字段 6 若为 string)。
    std::string name;
    /// POI 点标签(参考 xinzhi-map decodePoiFeature):
    /// minZoom/maxZoom = 显示级窗口(onset/hide),rank = 碰撞优先级。
    /// 仅 type 0/4 点标签层有;默认 minZoom=18/maxZoom=30/rank=0。
    int minZoom = 18;
    int maxZoom = 30;
    int rank = 0;
    /// POI 类别 subKey(PointFeatureSameStyle #2;默认 1)。
    int subKey = 1;
};

struct AmapDecodedLayerPart {
    int z = 0;
    int x = 0;
    int y = 0;
    int type = 0;
    std::vector<AmapDecodedFeature> features;
};

/// 解码一个 building/region/road 组(type=1 请求)容器。
/// 失败返回 false(长度头不匹配 / gzip 失败 / protobuf 畸形),error 可选。
bool decodeAmapTile(const uint8_t* data, size_t size,
                    std::vector<AmapDecodedLayerPart>& out,
                    std::string* error = nullptr);

/// POI 组(type=2 请求)容器。当前与主解码共用结构;标签类(0/1/4)的
/// name 抽取与几何语义(单点 unsigned 坐标)在 POI 专项接入时补全。
bool decodeAmapPoiTile(const uint8_t* data, size_t size,
                       std::vector<AmapDecodedLayerPart>& out,
                       std::string* error = nullptr);

}  // namespace earth_engine
