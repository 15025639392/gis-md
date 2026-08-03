#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

/// MVT(Mapbox Vector Tile 2.1)解码 DTO。
///
/// 坐标是瓦片本地整数(extent 空间,y 向下,可越界进 buffer 区),
/// 刻意不在解码层转经纬度——overzoom 切片(P4b)要在瓦片坐标上做,
/// 转 cartographic 归 bucketize 阶段(P4c)。
struct MvtPoint {
    int32_t x = 0;
    int32_t y = 0;

    bool operator==(const MvtPoint& rhs) const { return x == rhs.x && y == rhs.y; }
};

/// 与 MVT spec 的 GeomType 一一对应(Unknown 保留,消费方决定丢弃)。
enum class MvtGeomType : uint8_t {
    Unknown = 0,
    Point = 1,
    LineString = 2,
    Polygon = 3
};

struct MvtFeature {
    uint64_t id = 0;   ///< spec 可选;0 表示未携带(spec 保留 0 为缺省)
    MvtGeomType type = MvtGeomType::Unknown;

    /// 解码后的几何路径:
    /// Point: paths[0] = 全部点;LineString: 每条折线一个 path;
    /// Polygon: 每个环一个 path(未分类外环/孔,见 classifyMvtRings;
    ///          ClosePath 不重复首点,环隐式闭合)。
    std::vector<std::vector<MvtPoint>> paths;

    /// 属性表(Value 全部字符串化,对齐 Feature::properties 的约定:
    /// bool → "true"/"false",数值走最短往返表示)。
    std::unordered_map<std::string, std::string> properties;
};

struct MvtLayer {
    std::string name;
    uint32_t version = 1;
    uint32_t extent = 4096;
    std::vector<MvtFeature> features;
};

struct MvtTile {
    std::vector<MvtLayer> layers;
};

/// 环分类结果:一个面 = 一个外环 + 零或多个孔。
struct MvtPolygon {
    std::vector<MvtPoint> exterior;
    std::vector<std::vector<MvtPoint>> holes;
};

/// 解码一份 MVT 瓦片字节流。
///
/// 自动识别 gzip(1f 8b)/raw zlib(78 xx)压缩并解压。
/// 未知 protobuf 字段跳过(向前兼容);结构性损坏(varint 越界、
/// length 越界、几何命令流残缺)返回 false 并置 error。
/// 单个 feature 的几何命令非法只丢弃该 feature,不失败整瓦片
/// (与 osgearth/maplibre 的容错取向一致)。
bool decodeMvtTile(const uint8_t* data, size_t size, MvtTile& out,
                   std::string* error = nullptr);

/// 多边形环分类(对拍 @mapbox/vector-tile classifyRings 语义):
/// 首个非零有向面积环的绕向定义"外环绕向",其后同向环开新面、
/// 反向环挂当前面作孔;先于任何外环出现的孔按首环处理(容错,
/// 与 vector-tile-js 行为一致);零面积环丢弃。
/// 不依赖 spec 绕向约定,对绕向违规的编码器稳健。
std::vector<MvtPolygon> classifyMvtRings(
    const std::vector<std::vector<MvtPoint>>& rings);

/// 环有向面积 ×2(shoelace,瓦片 y 向下坐标系原样计算)。
/// 只用于绕向判断,消费方不应依赖其符号约定。
int64_t mvtRingSignedArea2(const std::vector<MvtPoint>& ring);

} // namespace earth_engine
