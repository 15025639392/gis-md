#pragma once

#include <string>

namespace earth_engine {

/// Cesium-native TileMapServiceRasterOverlay URL fallback:
/// resolve tilemapresource.xml relative to a TMS endpoint while preserving
/// query parameters and fragments.
std::string tileMapServiceXmlUrl(const std::string& url);

} // namespace earth_engine
