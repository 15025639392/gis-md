#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace earth_engine {

struct TileMapServiceTileSet {
    std::string url;
    uint32_t level = 0;
};

struct TileMapServiceMetadata {
    std::string fileExtension = "png";
    uint32_t tileWidth = 256;
    uint32_t tileHeight = 256;
    uint32_t minimumLevel = 0;
    uint32_t maximumLevel = 25;
    std::vector<TileMapServiceTileSet> tileSets;
};

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

TileMapServiceMetadata parseTileMapServiceMetadata(const std::string& xml);

} // namespace earth_engine
