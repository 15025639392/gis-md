#pragma once

#include "earth_engine/style/AmapClassicStyleOverride.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace earth_engine::minimal_globe_demo {

/// Parsed amap-vector.json (device getFilesDir). Each optional section leaves
/// its default values untouched when absent, so an omitted file or section
/// falls back to the sealed/official behavior.
struct AmapVectorConfig {
    // sources.amap — empty string = sealed official host.
    bool hasAmapEndpoints = false;
    std::string apiBase, initBase, iconBase, sdfBase;
    // sources.terrain — present = override scene terrain source.
    bool hasTerrain = false;
    std::string terrainUrlTemplate;
    int terrainMinZoom = 6;
    int terrainMaxZoom = 12;
    int terrainTileSize = 514;
    float terrainBorderInset = 0.5f;
    // zooms — present = override source discrete tiers / selection.
    bool hasZooms = false;
    int zoomMinZoom = 3;
    int zoomRegionsMaxZoom = 10;
    int zoomMainMaxZoom = 14;
    double zoomRegionsActiveBelowZoom = 12.0;
    std::vector<int> zoomRegionsSupported;
    std::vector<int> zoomMainSupported;
    std::vector<int> zoomPoiSupported;
    std::vector<std::pair<int, int>> zoomDataZoomRemap;
    // style — constant overrides on top of the sealed official contract.
    bool hasStyle = false;
    AmapClassicStyleOverrides styleOverrides;
};

/// Fail-loud parse. Returns empty string on success; otherwise a
/// human-readable error and the caller should log it and keep defaults.
/// Unknown keys inside known sections reject the whole file.
std::string parseAmapVectorConfig(const std::string& jsonText,
                                  AmapVectorConfig& out);

} // namespace earth_engine::minimal_globe_demo