#pragma once

#include "../layers/RasterOverlay.h"

#include <string>
#include <vector>

namespace earth_engine {

enum class TerrainSourceKind {
    None,
    HeightmapTerrainRgb,
    QuantizedMesh
};

enum class ImagerySourceKind {
    Debug,
    Xyz,
    TileMapService,
    WebMapService
};

struct SceneCameraConfig {
    double longitudeDegrees = 0.0;
    double latitudeDegrees = 0.0;
    double heightMeters = 0.0;
};

struct TerrainSourceConfig {
    TerrainSourceKind kind = TerrainSourceKind::None;
    std::string urlTemplate;
    std::string layerJsonUrl;
    std::string attribution;
    int minimumZoom = 0;
    int maximumZoom = 0;
    int tileSize = 0;
    bool flipYForUrl = false;
    bool enableWaterMask = false;
};

struct SceneTilesetConfig {
    double mainThreadLoadingTimeLimit = 0.0;
    double tileCacheUnloadTimeLimit = 0.0;
};

struct RasterOverlaySourceConfig {
    ImagerySourceKind imageryKind = ImagerySourceKind::Xyz;
    std::string urlTemplate;
    std::string tileMapResourceUrl;
    std::string attribution;
    int minimumZoom = 0;
    int maximumZoom = 0;
    int overlayMinimumZoom = 0;
    int overlayMaximumZoom = 0;
    int maximumSimultaneousTileLoads = 20;
    double maximumScreenSpaceError = 2.0;
    float opacity = 1.0f;
    RasterOverlayRole role = RasterOverlayRole::BaseImagery;
    RasterOverlayPriority priority = RasterOverlayPriority::High;
    RasterOverlayFallbackPolicy fallbackPolicy =
        RasterOverlayFallbackPolicy::AncestorOrPlaceholder;
    bool blocksCompleteRenderable = true;
    std::string wmsVersion = "1.3.0";
    std::string wmsLayers;
    std::string wmsFormat = "image/png";
    int imageryTileWidth = 0;
    int imageryTileHeight = 0;
};

struct GltfSourceConfig {
    bool enabled = false;
    std::string tileSchemeId = "Geographic-TMS";
    int tileLevel = 0;
    int tileX = 1;
    int tileY = 0;
    std::string url;
    std::string name;
    double longitudeDegrees = 0.0;
    double latitudeDegrees = 0.0;
    double heightMeters = 0.0;
    double uniformScale = 1.0;
};

struct EarthSceneConfig {
    /// Initial SDK scene camera. resetCamera() restores this viewpoint.
    SceneCameraConfig initialCamera;
    /// Terrain source. TerrainSourceKind::None keeps the ellipsoid surface.
    TerrainSourceConfig terrain;
    /// Tileset runtime tuning. Defaults keep core Tileset defaults.
    SceneTilesetConfig tileset;
    /// Ordered raster stack. Ownership becomes SDK runtime state on install.
    std::vector<RasterOverlaySourceConfig> rasterOverlays;
    /// Optional single glTF source, placed east-north-up on a configured tile.
    GltfSourceConfig gltf;
    /// Julian date set during installScene(); 0.0 leaves the epoch value.
    double fixedSimulationJulianDate = 0.0;
};

} // namespace earth_engine
