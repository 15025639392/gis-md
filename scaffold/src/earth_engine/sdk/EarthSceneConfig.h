#pragma once

#include "../layers/RasterOverlay.h"

#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace earth_engine {

enum class TerrainSourceKind {
    None,
    QuantizedMesh
};

enum class ImagerySourceKind {
    Debug,
    Xyz,
    TileMapService,
    WebMapService,
    WebMapTileService,
    BingMaps,
    GoogleMapTiles
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
    bool enableWaterMask = false;
};

struct SceneTilesetConfig {
    // 与 TilesetOptions::mainThreadLoadingTimeLimit 默认保持一致（8ms 时间闸门）。
    double mainThreadLoadingTimeLimit = 8.0;
    double tileCacheUnloadTimeLimit = 0.0;
    // LOD 过渡淡入淡出（cesium enableLodTransitionPeriod）。默认关，与
    // TilesetOptions 默认一致（cesium 原生出厂即关）。开启后瓦片切换 LOD 时
    // 新旧瓦片按 lodTransitionLength 秒 alpha 交叉淡入淡出，消除 pop。
    bool enableLodTransitionPeriod = false;
    float lodTransitionLength = 1.0f;
    // 运动期跳过快速划走的瓦片网络请求(cesium-js cullRequestsWhileMoving)。
    // 默认关 = 忠实 cesium-native。开启后拖动/缩放中减少瞬时加载洪泛,相机停下
    // 恢复正常加载。multiplier 越大剔除越激进(cesium 默认 60)。
    bool cullRequestsWhileMoving = false;
    double cullRequestsWhileMovingMultiplier = 60.0;
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
    std::string wmtsFormat;
    std::string wmtsLayer;
    std::string wmtsStyle;
    std::string wmtsTileMatrixSetId;
    std::string wmtsSchemeId = "XYZ-WebMercator";
    std::vector<std::string> wmtsTileMatrixLabels;
    std::vector<std::string> wmtsSubdomains;
    std::map<std::string, std::string> wmtsDimensions;
    std::string bingBaseUrl;
    std::string bingMapStyle = "Aerial";
    std::string bingKey;
    std::string bingCulture;
    std::vector<std::string> bingSubdomains;
    std::string googleMapTilesApiBaseUrl = "https://tile.googleapis.com/";
    std::string googleMapTilesKey;
    std::string googleMapTilesSession;
    bool googleMapTilesShowLogo = true;
    std::string googleMapTilesMapType = "satellite";
    std::string googleMapTilesLanguage = "en-US";
    std::string googleMapTilesRegion = "US";
    std::optional<std::string> googleMapTilesImageFormat;
    std::optional<std::string> googleMapTilesScale;
    std::optional<bool> googleMapTilesHighDpi;
    std::optional<std::vector<std::string>> googleMapTilesLayerTypes;
    std::optional<nlohmann::json> googleMapTilesStyles;
    std::optional<bool> googleMapTilesOverlay;
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
