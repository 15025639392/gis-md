#pragma once

namespace earth_engine {

/// 高德数据 zoom 档位(canonical z + 1 的 tier 表,参考同款):
/// 2-5→3, 6-7→6, 8-9→8, 10-12→10, 13-14→12, ≥15→14。
int amapDataZoom(int canonicalZ);

}  // namespace earth_engine
