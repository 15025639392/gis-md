#include "EarthEngineSdkFacade.h"

#include "../Engine.h"
#include "../content/GltfContentProvider.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../layers/RasterOverlay.h"
#include "../platform/bridge/PlatformBridge.h"
#include "../providers/BingMapsImageryProvider.h"
#include "../providers/DebugImageryProvider.h"
#include "../providers/GoogleMapTilesImageryProvider.h"
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

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
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

std::vector<uint8_t> postBlocking(
    PlatformBridge& platformBridge,
    const std::string& url,
    const std::string& body,
    const std::string& contentType,
    std::chrono::milliseconds timeout = std::chrono::seconds(20)) {
    struct State {
        std::vector<uint8_t> result;
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
    };
    auto state = std::make_shared<State>();
    std::vector<uint8_t> bytes(body.begin(), body.end());
    auto request = platformBridge.post(
        url,
        std::move(bytes),
        contentType,
        [state](int code, std::vector<uint8_t> response) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (code >= 200 && code < 300) {
                    state->result = std::move(response);
                }
                state->done = true;
            }
            state->cv.notify_one();
        },
        {HttpRequestPriority::High});

    bool done = false;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        done = state->cv.wait_for(lock, timeout, [&]() {
            return state->done;
        });
    }
    if (!done && request) {
        request->cancel();
    }
    return done ? std::move(state->result) : std::vector<uint8_t>{};
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
                    tileMapServiceTileBaseUrl(
                        overlayConfig.tileMapResourceUrl),
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
            wmtsOptions.schemeId = overlayConfig.wmtsSchemeId;
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
                overlayConfig.wmtsSchemeId == "Geographic-TMS"
                    ? TileScheme::createGeographicTMS()
                    : TileScheme::createXYZWebMercator(),
                makeRasterOverlayOptions(overlayConfig));
            continue;
        }

        if (overlayConfig.imageryKind == ImagerySourceKind::BingMaps) {
            if (!overlayConfig.urlTemplate.empty()) {
                BingMapsImageryOptions bingOptions;
                bingOptions.culture = overlayConfig.bingCulture;
                bingOptions.subdomains = overlayConfig.bingSubdomains;
                bingOptions.minimumLevel = overlayConfig.minimumZoom;
                bingOptions.maximumLevel = overlayConfig.maximumZoom;
                if (overlayConfig.imageryTileWidth > 0) {
                    bingOptions.tileWidth = overlayConfig.imageryTileWidth;
                }
                if (overlayConfig.imageryTileHeight > 0) {
                    bingOptions.tileHeight = overlayConfig.imageryTileHeight;
                }

                auto bing = std::make_unique<BingMapsImageryProvider>(
                    overlayConfig.bingBaseUrl,
                    overlayConfig.urlTemplate,
                    std::move(bingOptions),
                    overlayConfig.attribution);
                bing->setPlatformBridge(&platformBridge_);
                addActivatedRasterOverlay(
                    rasterOverlays,
                    std::move(bing),
                    TileScheme::createXYZWebMercator(),
                    makeRasterOverlayOptions(overlayConfig));
                continue;
            }

            const std::string metadataUrl = bingMapsMetadataUrl(
                overlayConfig.bingBaseUrl,
                overlayConfig.bingMapStyle,
                overlayConfig.bingKey,
                overlayConfig.bingCulture);
            QuantizedMeshLayerJsonFetcher fetcher(&platformBridge_);
            const std::vector<uint8_t> bytes = fetcher.fetchBlocking(
                metadataUrl);
            if (bytes.empty()) {
                logError(platformBridge_,
                         "Bing Maps metadata load failed: " + metadataUrl);
                continue;
            }

            BingMapsMetadataParseResult metadata = parseBingMapsMetadata(
                std::string(bytes.begin(), bytes.end()));
            if (!metadata.valid) {
                logError(platformBridge_,
                         "Bing Maps metadata validation failed: " +
                             metadata.error);
                continue;
            }

            BingMapsImagerySource source = createBingMapsImagerySource(
                overlayConfig.bingBaseUrl,
                metadata.metadata,
                overlayConfig.bingCulture,
                overlayConfig.attribution);
            if (!source.provider || !source.scheme) {
                logError(platformBridge_,
                         "Bing Maps metadata did not create a provider: " +
                             metadataUrl);
                continue;
            }
            source.provider->setPlatformBridge(&platformBridge_);
            addActivatedRasterOverlay(rasterOverlays,
                                      std::move(source.provider),
                                      std::move(source.scheme),
                                      makeRasterOverlayOptions(overlayConfig));
            continue;
        }

        if (overlayConfig.imageryKind == ImagerySourceKind::GoogleMapTiles) {
            GoogleMapTilesExistingSessionOptions googleOptions;
            if (overlayConfig.googleMapTilesSession.empty()) {
                GoogleMapTilesNewSessionOptions requestOptions;
                requestOptions.apiBaseUrl =
                    overlayConfig.googleMapTilesApiBaseUrl;
                requestOptions.key = overlayConfig.googleMapTilesKey;
                requestOptions.mapType = overlayConfig.googleMapTilesMapType;
                requestOptions.language =
                    overlayConfig.googleMapTilesLanguage;
                requestOptions.region = overlayConfig.googleMapTilesRegion;
                requestOptions.imageFormat =
                    overlayConfig.googleMapTilesImageFormat;
                requestOptions.scale = overlayConfig.googleMapTilesScale;
                requestOptions.highDpi = overlayConfig.googleMapTilesHighDpi;
                requestOptions.layerTypes =
                    overlayConfig.googleMapTilesLayerTypes;
                requestOptions.styles = overlayConfig.googleMapTilesStyles;
                requestOptions.overlay = overlayConfig.googleMapTilesOverlay;

                const std::string createSessionUrl =
                    googleMapTilesCreateSessionUrl(requestOptions);
                const std::string payload =
                    googleMapTilesCreateSessionPayload(requestOptions);
                const std::vector<uint8_t> bytes = postBlocking(
                    platformBridge_,
                    createSessionUrl,
                    payload,
                    "application/json");
                if (bytes.empty()) {
                    logError(platformBridge_,
                             "Google Map Tiles createSession load failed: " +
                                 createSessionUrl);
                    continue;
                }

                GoogleMapTilesSessionParseResult session =
                    parseGoogleMapTilesCreateSessionResponse(
                        std::string(bytes.begin(), bytes.end()),
                        requestOptions);
                if (!session.valid) {
                    logError(platformBridge_,
                             "Google Map Tiles createSession validation failed: " +
                                 session.error);
                    continue;
                }
                googleOptions = std::move(session.session);
            } else {
                googleOptions.apiBaseUrl =
                    overlayConfig.googleMapTilesApiBaseUrl;
                googleOptions.key = overlayConfig.googleMapTilesKey;
                googleOptions.session = overlayConfig.googleMapTilesSession;
                if (overlayConfig.imageryTileWidth > 0) {
                    googleOptions.tileWidth = overlayConfig.imageryTileWidth;
                }
                if (overlayConfig.imageryTileHeight > 0) {
                    googleOptions.tileHeight = overlayConfig.imageryTileHeight;
                }
            }
            googleOptions.showLogo = overlayConfig.googleMapTilesShowLogo;
            googleOptions.maximumLevel =
                overlayConfig.maximumZoom > 0 ? overlayConfig.maximumZoom : 28;

            GoogleMapTilesImagerySource source =
                createGoogleMapTilesImagerySource(
                    std::move(googleOptions),
                    overlayConfig.attribution);
            if (!source.provider || !source.scheme) {
                logError(platformBridge_,
                         "Google Map Tiles existing session did not create a provider");
                continue;
            }
            source.provider->setPlatformBridge(&platformBridge_);
            source.provider->loadCredits();
            addActivatedRasterOverlay(rasterOverlays,
                                      std::move(source.provider),
                                      std::move(source.scheme),
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
