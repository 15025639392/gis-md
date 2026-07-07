#pragma once

#include "FeatureStore.h"

#include <string>

namespace earth_engine {

/// GeoJSON → FeatureStore 导入器。
///
/// 复用 GeoJsonParser 解析(整文件 → GeoFeature),把每个 GeoFeature 提升为
/// 权威 Feature(分配稳定 ID、保留源 ID、拷属性/包围盒),灌进 FeatureStore。
/// Multi* 类型在 parser 已分解为多个单几何 feature。
class GeoJsonImporter {
public:
    /// 解析并导入 GeoJSON 文本,返回成功导入的要素数。
    /// 解析失败(空/非法 JSON)返回 0,不改动 store。
    static size_t importInto(const std::string& geoJsonText, FeatureStore& store);
};

} // namespace earth_engine
