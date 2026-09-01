#pragma once

#include "../core/geodesy/Cartographic.h"
#include "../core/math/Rectangle.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

/// 稳定全局要素 ID，由 FeatureStore 分配。0 表示无效。
using FeatureId = uint64_t;
inline constexpr FeatureId kInvalidFeatureId = 0;

/// 矢量要素几何类型（与 GeoJsonParser 的 DTO 解耦，存储层独立）。
enum class GeometryType {
    Point,
    LineString,
    Polygon
};

/// 权威可编辑要素。
///
/// 与解析用的 GeoFeature（DTO）不同,Feature 是 FeatureStore 的一等存储对象:
/// 全精度 cartographic 环(不量化、不按瓦片裁剪)、稳定全局 ID、属性表、版本号。
/// 版本号服务 undo/redo(纯本地,无后端同步)。
struct Feature {
    FeatureId id = kInvalidFeatureId;   ///< store 分配的稳定 ID
    std::string sourceId;               ///< 源数据里的 feature.id 原值(可空)
    GeometryType type = GeometryType::Point;

    /// 几何环(radian + meter),约定同 GeoFeature:
    /// Point: rings[0][0];  LineString: rings[0];  Polygon: rings[0]=外环, rings[1..]=孔
    std::vector<std::vector<Cartographic>> rings;

    std::unordered_map<std::string, std::string> properties;

    /// Provider-authored UTF-16 split indices for label layout. Kept out
    /// of the visible name string so identity and provider text stay exact.
    std::vector<uint32_t> labelSplitIndicesUtf16;

    /// 经纬度包围盒(radian)。约定为非跨反经线矩形(west <= east)。
    Rectangle bounds;

    /// 每次编辑递增,服务 undo/redo。
    uint64_t version = 1;
};

} // namespace earth_engine
