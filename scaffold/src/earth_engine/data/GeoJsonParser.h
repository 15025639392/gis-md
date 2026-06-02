#pragma once

#include "../core/geodesy/Cartographic.h"
#include "../core/math/Rectangle.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace earth_engine {

/// 解析后的 GeoJSON Feature。
/// 坐标已从 [lng°, lat°] 转换为 radian。
/// Multi* 类型在解析时分解为多个单几何 Feature。
struct GeoFeature {
    enum class Type {
        Point,
        LineString,
        Polygon
    };

    std::string id;          // feature.id 或自动生成
    Type type;

    /// 几何环（radian + meter）
    /// Point:       rings[0][0] 是唯一点
    /// LineString:  rings[0] 是顶点序列
    /// Polygon:     rings[0] 是外环，rings[1..] 是孔（holes）
    std::vector<std::vector<Cartographic>> rings;

    /// 属性表（均为字符串，VectorLayer 按需解析）
    std::unordered_map<std::string, std::string> properties;

    /// 经纬度包围盒（radian）
    Rectangle bounds;
};

/// GeoJSON 解析器。
/// 坐标从 [lng°, lat°] 转换为 radian，height 单位为米。
class GeoJsonParser {
public:
    /// 解析 GeoJSON 字符串为 feature 列表。
    /// Multi* 类型会分解为多个独立的单几何 feature。
    /// @param jsonText GeoJSON FeatureCollection 文本（UTF-8）
    /// @return 解析后的 feature 列表；解析失败返回空列表
    static std::vector<GeoFeature> parse(const std::string& jsonText);
};

} // namespace earth_engine
