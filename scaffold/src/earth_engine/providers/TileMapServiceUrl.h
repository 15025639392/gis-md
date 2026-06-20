#pragma once

#include <string>

namespace earth_engine {

/// Cesium-native TileMapServiceRasterOverlay URL fallback:
/// resolve tilemapresource.xml relative to a TMS endpoint while preserving
/// query parameters and fragments.
std::string tileMapServiceXmlUrl(const std::string& url);

/// Cesium-native TileMapServiceTileProvider tile URL:
/// resolve "<tileSetUrl>/<x>/<y><fileExtension>" relative to the TMS base URL.
std::string tileMapServiceTileUrl(const std::string& baseUrl,
                                  const std::string& tileSetUrl,
                                  int x,
                                  int y,
                                  const std::string& fileExtension);

} // namespace earth_engine
