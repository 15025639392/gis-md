#include "EarthEngineSdkFacade.h"

#include "../Engine.h"
#include "../content/GltfContentProvider.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../layers/RasterOverlay.h"
#include "../platform/bridge/PlatformBridge.h"
#include "../providers/DebugImageryProvider.h"
#include "../providers/HeightmapTerrainProvider.h"
#include "../providers/QuantizedMeshLayerJsonFetcher.h"
#include "../providers/QuantizedMeshTerrainProvider.h"
#include "../providers/TileMapServiceImageryProvider.h"
#include "../providers/TileMapServiceUrl.h"
#include "../providers/WebMapServiceImageryProvider.h"
#include "../providers/WebMapTileServiceImageryProvider.h"
#include "../providers/XYZImageryProvider.h"
#include "../renderer/RenderDevice.h"
#include "../scene/Camera.h"
#include "../tiling/Tileset.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace earth_engine {
namespace {

TilesetOptions makeSceneTilesetOptions(const SceneTilesetConfig& config) {
    TilesetOptions options;
    options.mainThreadLoadingTimeLimit = config.mainThreadLoadingTimeLimit;
    options.tileCacheUnloadTimeLimit = config.tileCacheUnloadTimeLimit;
    return options;
}

RasterOverlay::Options makeRasterOverlayOptions(
    const RasterOverlaySourceConfig& config) {
    RasterOverlay::Options options{};
    options.maximumSimultaneousTileLoads =
        config.maximumSimultaneousTileLoads;
    options.maximumScreenSpaceError = config.maximumScreenSpaceError;
    options.minimumZoom = config.overlayMinimumZoom;
    options.maximumZoom = config.overlayMaximumZoom;
    options.visible = true;
    options.opacity = config.opacity;
    options.role = config.role;
    options.priority = config.priority;
    options.fallbackPolicy = config.fallbackPolicy;
    options.blocksCompleteRenderable = config.blocksCompleteRenderable;
    return options;
}

void logInfo(PlatformBridge& platformBridge, const std::string& message) {
    platformBridge.log(LogLevel::Info, "EarthEngineSdk", message);
}

void logError(PlatformBridge& platformBridge, const std::string& message) {
    platformBridge.log(LogLevel::Error, "EarthEngineSdk", message);
}

template <typename Provider>
void applyConfiguredZoomRange(Provider& provider,
                              int minimumZoom,
                              int maximumZoom) {
    if (minimumZoom <= 0 && maximumZoom <= 0) {
        return;
    }

    provider.setZoomRange(minimumZoom,
                          maximumZoom > 0 ? maximumZoom : provider.maxZoom());
}

std::unique_ptr<TerrainProvider> createTerrainProvider(
    const TerrainSourceConfig& config,
    PlatformBridge& platformBridge) {
    if (config.kind == TerrainSourceKind::None) {
        return {};
    }

    if (config.kind == TerrainSourceKind::QuantizedMesh) {
        auto qm = std::make_unique<QuantizedMeshTerrainProvider>(
            config.urlTemplate, config.attribution);
        applyConfiguredZoomRange(*qm, config.minimumZoom, config.maximumZoom);
        qm->setTileSize(config.tileSize);
        qm->setFlipYForUrl(config.flipYForUrl);
        qm->setWaterMaskEnabled(config.enableWaterMask);
        qm->setPlatformBridge(&platformBridge);
        if (!qm->configureFromLayerJsonUrl(config.layerJsonUrl)) {
            logError(platformBridge,
                     "QuantizedMesh layer.json load failed: " +
                         config.layerJsonUrl);
        }
        return qm;
    }

    auto hm = std::make_unique<HeightmapTerrainProvider>(
        config.urlTemplate, config.attribution);
    applyConfiguredZoomRange(*hm, config.minimumZoom, config.maximumZoom);
    hm->setEncoding(HeightmapTerrainProvider::Encoding::MapboxTerrainRgb);
    hm->setTileSize(config.tileSize);
    hm->setPlatformBridge(&platformBridge);
    return hm;
}

} // namespace

EarthEngineSdkFacade::EarthEngineSdkFacade(Engine& engine,
                                           RenderDevice& renderDevice,
                                           PlatformBridge& platformBridge)
    : engine_(engine),
      renderDevice_(renderDevice),
      platformBridge_(platformBridge) {}

EarthEngineSdkFacade::~EarthEngineSdkFacade() = default;

void EarthEngineSdkFacade::installScene(EarthSceneConfig config) {
    config_ = std::move(config);
    resetCamera();

    rasterOverlays_.clear();
    activatedRasterOverlays_.clear();

    std::vector<ActivatedRasterOverlay*> rasterOverlays;
    for (const RasterOverlaySourceConfig& overlayConfig :
         config_.rasterOverlays) {
        if (overlayConfig.imageryKind == ImagerySourceKind::Debug) {
            auto dbg = std::make_unique<DebugImageryProvider>();
            addActivatedRasterOverlay(
                rasterOverlays,
                std::move(dbg),
                TileScheme::createXYZWebMercator(),
                makeRasterOverlayOptions(overlayConfig));
            continue;
        }

        if (overlayConfig.imageryKind == ImagerySourceKind::TileMapService) {
            const std::string xmlUrl =
                tileMapServiceXmlUrl(overlayConfig.tileMapResourceUrl);
            QuantizedMeshLayerJsonFetcher fetcher(&platformBridge_);
            const std::vector<uint8_t> bytes = fetcher.fetchBlocking(xmlUrl);
            if (bytes.empty()) {
                logError(platformBridge_,
                         "TMS tilemapresource.xml load failed: " + xmlUrl);
                continue;
            }

            TileMapServiceImagerySource source =
                createTileMapServiceImagerySource(
                    xmlUrl,
                    std::string(bytes.begin(), bytes.end()),
                    overlayConfig.attribution);
            if (!source.provider || !source.scheme) {
                logError(platformBridge_,
                         "TMS tilemapresource.xml has no usable tilesets: " +
                             xmlUrl);
                continue;
            }

            applyConfiguredZoomRange(*source.provider,
                                     overlayConfig.minimumZoom,
                                     overlayConfig.maximumZoom);
            source.provider->setPlatformBridge(&platformBridge_);

            RasterOverlay::Options options =
                makeRasterOverlayOptions(overlayConfig);
            if (source.coverageRectangle) {
                options.coverageRectangle = *source.coverageRectangle;
            }
            addActivatedRasterOverlay(rasterOverlays,
                                      std::move(source.provider),
                                      std::move(source.scheme),
                                      options);
            continue;
        }

        if (overlayConfig.imageryKind == ImagerySourceKind::WebMapService) {
            WebMapServiceImageryOptions wmsOptions;
            wmsOptions.version = overlayConfig.wmsVersion;
            wmsOptions.layers = overlayConfig.wmsLayers;
            wmsOptions.format = overlayConfig.wmsFormat;
            wmsOptions.minimumLevel = overlayConfig.minimumZoom;
            wmsOptions.maximumLevel = overlayConfig.maximumZoom;
            if (overlayConfig.imageryTileWidth > 0) {
                wmsOptions.tileWidth = overlayConfig.imageryTileWidth;
            }
            if (overlayConfig.imageryTileHeight > 0) {
                wmsOptions.tileHeight = overlayConfig.imageryTileHeight;
            }

            const std::string capabilitiesUrl =
                webMapServiceCapabilitiesUrl(overlayConfig.urlTemplate,
                                             wmsOptions.version);
            QuantizedMeshLayerJsonFetcher fetcher(&platformBridge_);
            const std::vector<uint8_t> bytes =
                fetcher.fetchBlocking(capabilitiesUrl);
            if (bytes.empty()) {
                logError(platformBridge_,
                         "WMS GetCapabilities load failed: " +
                             capabilitiesUrl);
                continue;
            }

            const WebMapServiceCapabilitiesValidation validation =
                validateWebMapServiceCapabilities(
                    std::string(bytes.begin(), bytes.end()),
                    wmsOptions);
            if (!validation.valid) {
                logError(platformBridge_,
                         "WMS GetCapabilities validation failed: " +
                             validation.error);
                continue;
            }

            auto wms = std::make_unique<WebMapServiceImageryProvider>(
                overlayConfig.urlTemplate,
                std::move(wmsOptions),
                overlayConfig.attribution);
            wms->setPlatformBridge(&platformBridge_);
            addActivatedRasterOverlay(
                rasterOverlays,
                std::move(wms),
                TileScheme::createGeographicTMS(),
                makeRasterOverlayOptions(overlayConfig));
            continue;
        }

        if (overlayConfig.imageryKind == ImagerySourceKind::WebMapTileService) {
            WebMapTileServiceImageryOptions wmtsOptions;
            if (!overlayConfig.wmtsFormat.empty()) {
                wmtsOptions.format = overlayConfig.wmtsFormat;
            }
            wmtsOptions.layer = overlayConfig.wmtsLayer;
            wmtsOptions.style = overlayConfig.wmtsStyle;
            wmtsOptions.tileMatrixSetId =
                overlayConfig.wmtsTileMatrixSetId;
            wmtsOptions.tileMatrixLabels =
                overlayConfig.wmtsTileMatrixLabels.empty()
                    ? std::optional<std::vector<std::string>>()
                    : std::make_optional(overlayConfig.wmtsTileMatrixLabels);
            wmtsOptions.subdomains = overlayConfig.wmtsSubdomains;
            wmtsOptions.dimensions = overlayConfig.wmtsDimensions.empty()
                ? std::optional<std::map<std::string, std::string>>()
                : std::make_optional(overlayConfig.wmtsDimensions);
            wmtsOptions.minimumLevel = overlayConfig.minimumZoom;
            wmtsOptions.maximumLevel = overlayConfig.maximumZoom;
            if (overlayConfig.imageryTileWidth > 0) {
                wmtsOptions.tileWidth = overlayConfig.imageryTileWidth;
            }
            if (overlayConfig.imageryTileHeight > 0) {
                wmtsOptions.tileHeight = overlayConfig.imageryTileHeight;
            }

            auto wmts = std::make_unique<WebMapTileServiceImageryProvider>(
                overlayConfig.urlTemplate,
                std::move(wmtsOptions),
                overlayConfig.attribution);
            wmts->setPlatformBridge(&platformBridge_);
            addActivatedRasterOverlay(
                rasterOverlays,
                std::move(wmts),
                TileScheme::createXYZWebMercator(),
                makeRasterOverlayOptions(overlayConfig));
            continue;
        }

        auto xyz = std::make_unique<XYZImageryProvider>(
            overlayConfig.urlTemplate, overlayConfig.attribution);
        applyConfiguredZoomRange(
            *xyz,
            overlayConfig.minimumZoom,
            overlayConfig.maximumZoom);
        xyz->setPlatformBridge(&platformBridge_);
        addActivatedRasterOverlay(
            rasterOverlays,
            std::move(xyz),
            TileScheme::createXYZWebMercator(),
            makeRasterOverlayOptions(overlayConfig));
    }

    const TilesetOptions tilesetOptions =
        makeSceneTilesetOptions(config_.tileset);
    auto tileset = std::make_unique<Tileset>(
        createTerrainProvider(config_.terrain, platformBridge_),
        TileScheme::createGeographicTMS(),
        std::move(rasterOverlays),
        &renderDevice_,
        tilesetOptions);
    engine_.setTileset(std::move(tileset));
    logInfo(platformBridge_, "Unified Tileset created");

    if (config_.gltf.enabled) {
        const TileKey gltfKey{
            config_.gltf.tileSchemeId,
            config_.gltf.tileLevel,
            config_.gltf.tileX,
            config_.gltf.tileY};
        auto gltfProvider = std::make_unique<SingleGltfContentProvider>(
            gltfKey,
            config_.gltf.url,
            config_.gltf.name);
        gltfProvider->setPlatformBridge(&platformBridge_);
        gltfProvider->setEastNorthUpPlacementDegrees(
            config_.gltf.longitudeDegrees,
            config_.gltf.latitudeDegrees,
            config_.gltf.heightMeters,
            config_.gltf.uniformScale);

        auto gltfTileset = std::make_unique<Tileset>(
            std::unique_ptr<TerrainProvider>{},
            TileScheme::createGeographicTMS(),
            std::vector<ActivatedRasterOverlay*>{},
            &renderDevice_,
            tilesetOptions,
            std::move(gltfProvider));
        engine_.addTileset(std::move(gltfTileset));
        logInfo(platformBridge_, "glTF tileset added: " + config_.gltf.url);
    }

    engine_.setTime(config_.fixedSimulationJulianDate);
}

void EarthEngineSdkFacade::resetCamera() {
    const auto& ellipsoid = Ellipsoid::WGS84();
    auto targetEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromDegrees(config_.initialCamera.longitudeDegrees,
                                  config_.initialCamera.latitudeDegrees,
                                  0.0));
    auto camEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromDegrees(config_.initialCamera.longitudeDegrees,
                                  config_.initialCamera.latitudeDegrees,
                                  config_.initialCamera.heightMeters));
    Vec3 up = ellipsoid.geodeticSurfaceNormal(targetEcef);
    engine_.camera().lookAt(camEcef, targetEcef, up);
}

void EarthEngineSdkFacade::addActivatedRasterOverlay(
    std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    std::unique_ptr<ImageryProvider> provider,
    std::unique_ptr<TileScheme> scheme,
    RasterOverlay::Options options) {
    auto overlay = std::make_unique<RasterOverlay>(
        std::move(provider),
        std::move(scheme),
        options);
    auto active = std::make_unique<ActivatedRasterOverlay>(*overlay);

    rasterOverlays.push_back(active.get());
    rasterOverlays_.push_back(std::move(overlay));
    activatedRasterOverlays_.push_back(std::move(active));
}

} // namespace earth_engine
