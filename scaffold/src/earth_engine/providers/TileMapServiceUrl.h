#pragma once

#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/tiling/TileKey.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace earth_engine {

class TileScheme;

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
    std::string schemeId = "TMS-WebMercator";
    bool boundingBoxCoordinatesInDegrees = false;
    std::optional<Rectangle> projectedCoverageRectangle;
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

/// Cesium-native TileMapServiceTileProvider level lookup:
/// level index is tile.z - minimumLevel; missing tile sets produce no URL.
std::optional<std::string> tileMapServiceTileUrlForKey(
    const std::string& baseUrl,
    const TileMapServiceMetadata& metadata,
    const TileKey& key);

/// Convert cesium-native projected TMS coverage into the geographic-radian
/// rectangle used by RasterOverlay options.
std::optional<Rectangle> tileMapServiceGeographicCoverageRectangle(
    const TileMapServiceMetadata& metadata);

std::unique_ptr<TileScheme> tileMapServiceTileScheme(
    const TileMapServiceMetadata& metadata);

TileMapServiceMetadata parseTileMapServiceMetadata(const std::string& xml);

} // namespace earth_engine
