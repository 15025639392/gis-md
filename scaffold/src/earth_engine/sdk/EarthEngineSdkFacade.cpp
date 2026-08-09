#include "EarthEngineSdkFacade.h"

#include "../Engine.h"
#include "../content/CompositeTerrainProvider.h"
#include "../content/EllipsoidTerrainContentProvider.h"
#include "../content/GltfContentProvider.h"
#include "../content/HeightmapTerrainContentProvider.h"
#include "../core/geodesy/Cartographic.h"
#include "../debug/Contracts.h"
#include "../debug/EnvSnapshot.h"
#include "../debug/PlatformLog.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../layers/RasterOverlay.h"
#include "../camera/CameraController.h"
#include "../platform/bridge/PlatformBridge.h"
#include "../providers/BingMapsImageryProvider.h"
#include "../providers/BlockingHttpFetcher.h"
#include "../providers/DebugImageryProvider.h"
#include "../providers/GoogleMapTilesImageryProvider.h"
#include "../providers/TileMapServiceImageryProvider.h"
#include "../providers/TileMapServiceUrl.h"
#include "../providers/WebMapServiceImageryProvider.h"
#include "../providers/WebMapTileServiceImageryProvider.h"
#include "../providers/XYZImageryProvider.h"
#include "../renderer/RenderDevice.h"
#include "../scene/Camera.h"
#include "../tiling/Tileset.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cmath>
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
    options.maximumCachedBytes = config.maximumCachedBytes;
    options.cullRequestsWhileMoving = config.cullRequestsWhileMoving;
    options.cullRequestsWhileMovingMultiplier =
        config.cullRequestsWhileMovingMultiplier;
    options.enableTerrainFillProxy = config.enableTerrainFillProxy;
    options.terrainFillProxyGridSize = config.terrainFillProxyGridSize;
    options.decoupleImageryFromGeometry = config.decoupleImageryFromGeometry;
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
    options.georeference = config.georeference;
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

std::unique_ptr<TileScheme> createTileSchemeForId(
    const std::string& schemeId) {
    if (schemeId == "XYZ-WebMercator") {
        return TileScheme::createXYZWebMercator();
    }
    return TileScheme::createGeographicTMS();
}

struct SceneTerrainRuntimeSources {
    std::unique_ptr<TilesetContentProvider> contentProvider;
    std::unique_ptr<TileScheme> tileScheme =
        TileScheme::createGeographicTMS();
};

SceneTerrainRuntimeSources createTerrainRuntimeSources(
    const TerrainSourceConfig& config,
    PlatformBridge& platformBridge) {
    SceneTerrainRuntimeSources sources;
    if (config.kind == TerrainSourceKind::None) {
        return sources;
    }

    if (config.kind == TerrainSourceKind::Heightmap) {
        const HeightmapTerrainProvider::Encoding encoding =
            config.heightmapEncoding == TerrainHeightmapEncoding::Terrarium
                ? HeightmapTerrainProvider::Encoding::Terrarium
                : HeightmapTerrainProvider::Encoding::MapboxTerrainRgb;
        auto hm = std::make_unique<HeightmapTerrainProvider>(
            config.urlTemplate, config.attribution);
        hm->setEncoding(encoding);
        hm->setPlatformBridge(&platformBridge);
        const int maxZoom = config.maximumZoom > 0 ? config.maximumZoom : 14;
        hm->setZoomRange(config.minimumZoom, maxZoom);
        if (config.heightmapMaxNativeZoom > 0) {
            hm->setMaxNativeZoom(config.heightmapMaxNativeZoom);
        }
        if (config.tileSize > 0) {
            hm->setTileSize(config.tileSize);
        }
        hm->setHeightFactor(config.heightmapHeightFactor);
        hm->setBorderInset(config.heightmapBorderInset);
        if (!config.heightmapNoDataValues.empty()) {
            hm->setNoDataValues(config.heightmapNoDataValues);
        }
        const std::string schemeId = hm->schemeId();
        sources.tileScheme = createTileSchemeForId(schemeId);
        auto content = std::make_unique<HeightmapTerrainContentProvider>(
            std::move(hm), maxZoom);
        if (config.ellipsoidFallback) {
            // Ellipsoid floor for uncovered regions, sharing the tiling scheme
            // so both quadtrees align.
            auto ellipsoid = std::make_unique<EllipsoidTerrainContentProvider>(
                schemeId,
                config.ellipsoidFallbackMaxZoom,
                config.ellipsoidFallbackGridSize);
            sources.contentProvider =
                std::make_unique<CompositeTerrainProvider>(
                    std::move(content),
                    std::move(ellipsoid),
                    config.ellipsoidFallbackMaxZoom);
        } else {
            sources.contentProvider = std::move(content);
        }
        return sources;
    }

    logError(platformBridge, "Unsupported terrain source kind");
    return sources;
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
            BlockingHttpFetcher fetcher(&platformBridge_);
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
            BlockingHttpFetcher fetcher(&platformBridge_);
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
            BlockingHttpFetcher fetcher(&platformBridge_);
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

    // 应用自建 overlay 排在配置 overlay 之后 = 叠在卫星影像之上。
    for (PendingCustomOverlay& pending : pendingCustomOverlays_) {
        if (!pending.provider || !pending.scheme) continue;
        addActivatedRasterOverlay(rasterOverlays, std::move(pending.provider),
                                  std::move(pending.scheme), pending.options);
    }
    pendingCustomOverlays_.clear();

    const TilesetOptions tilesetOptions =
        makeSceneTilesetOptions(config_.tileset);
    SceneTerrainRuntimeSources terrainSources =
        createTerrainRuntimeSources(config_.terrain, platformBridge_);
    auto tileset = std::make_unique<Tileset>(
        std::move(terrainSources.tileScheme),
        std::move(rasterOverlays),
        &renderDevice_,
        tilesetOptions,
        std::move(terrainSources.contentProvider));
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
        // 世界锚定内容(i3dm 绝对 ECEF)不套 ENU 就地放置——内容自身已定位,
        // 再叠 ENU 会二次平移把它推到界外(见 GltfSourceConfig::worldAnchored)。
        if (!config_.gltf.worldAnchored) {
            gltfProvider->setEastNorthUpPlacementDegrees(
                config_.gltf.longitudeDegrees,
                config_.gltf.latitudeDegrees,
                config_.gltf.heightMeters,
                config_.gltf.uniformScale);
        }

        auto gltfTileset = std::make_unique<Tileset>(
            TileScheme::createGeographicTMS(),
            std::vector<ActivatedRasterOverlay*>{},
            &renderDevice_,
            tilesetOptions,
            std::move(gltfProvider));
        engine_.addTileset(std::move(gltfTileset));
        logInfo(platformBridge_, "glTF tileset added: " + config_.gltf.url);
    }

    engine_.setTime(config_.fixedSimulationJulianDate);
    // 效果 setter 在后端不支持时拒绝并返回 false;这里把"配置了但没生效"
    // 汇总成一条显式告警,避免 iOS 上开关静默无效被误判为视觉不明显。
    const bool passthroughOk =
        engine_.setOffscreenPassthroughEnabled(config_.debugOffscreenPassthrough);
    const bool fxaaOk = engine_.setFxaaEnabled(config_.fxaa);
    const bool fogOk = engine_.setAerialFogEnabled(config_.aerialFog);
    if (!passthroughOk || !fxaaOk || !fogOk) {
        platformLog(LogLevel::Warning, "SdkFacade",
                    "offscreen post-process effects requested by scene config "
                    "were rejected by the render backend (see Engine errors)");
    }
    engine_.setAerialFogParams(config_.aerialFogDensity,
                               config_.aerialFogStartDistance);
    engine_.setVirtualTexturePocEnabled(config_.virtualTexturePoc);
    engine_.setTileCompositeBakePocEnabled(config_.tileCompositeBakePoc);
    engine_.setVtIndirectionSamplePocEnabled(config_.vtIndirectionSamplePoc);
    engine_.setTerrainPageStoreEnabled(config_.terrainPageStore);
    engine_.setGpuPassTimingEnabled(config_.gpuPassTiming);
    engine_.setFrameGatingEnabled(config_.frameGating);
    engine_.setShadowVerifyEnabled(config_.shadowVerifyIdle);

    // 环境快照 boot 段:场景装好、开关最终值已定(效果 setter 可能被后端拒绝)
    // 之后打一次。放这里而不是 installScene 开头,是为了让 fxaa/fog 报的是
    // **生效值**而非请求值。字段各自杀掉过哪次误判见 EnvSnapshot.h。
    {
        envsnap::BootFields env;
        switch (renderDevice_.backendType()) {
            case RenderDevice::Backend::Metal:    env.backend = "Metal"; break;
            case RenderDevice::Backend::OpenGLES: env.backend = "GLES";  break;
            case RenderDevice::Backend::Vulkan:   env.backend = "Vulkan"; break;
        }
        const TerrainSourceConfig& terrain = config_.terrain;
        if (terrain.kind != TerrainSourceKind::None) {
            env.demUrl = terrain.urlTemplate.c_str();
            env.demMinZoom = terrain.minimumZoom;
            env.demMaxZoom = terrain.maximumZoom;
            env.demBorderInset = terrain.heightmapBorderInset;
            // 报**解码时实际生效**的哨兵数,不是配置列表长度:Terrain-RGB 的
            // -10000 由解码器隐式注册,直接报配置长度会在哨兵已就位时显示 0
            // ——恰好在这个字段唯一该报警的场景里给出假绿灯。判据单一来源见
            // HeightmapTerrainProvider::hasImplicitNoDataSentinel。
            env.nodataRegisteredCount =
                HeightmapTerrainProvider::effectiveNoDataCount(
                    terrain.heightmapEncoding == TerrainHeightmapEncoding::Terrarium
                        ? HeightmapTerrainProvider::Encoding::Terrarium
                        : HeightmapTerrainProvider::Encoding::MapboxTerrainRgb,
                    terrain.heightmapNoDataValues.size());
        }
        env.overlayCount = static_cast<int>(config_.rasterOverlays.size());
        if (!config_.rasterOverlays.empty()) {
            const RasterOverlaySourceConfig& first = config_.rasterOverlays.front();
            env.overlayUrl = first.urlTemplate.c_str();
            env.overlayMinZoom = first.minimumZoom;
            env.overlayMaxZoom = first.maximumZoom;
        }
        // 投影读**运行期 overlay**(activatedRasterOverlays_)而不是 config:
        // georeference 是请求值,scheme 闸口可能拒掉它,而被拒后的画面与「没配」
        // 无法区分。字段语义见 EnvSnapshot.h。
        if (!activatedRasterOverlays_.empty()) {
            env.overlayProjection = rasterOverlayProjectionName(
                activatedRasterOverlays_.front()->getProjection());
            for (const std::unique_ptr<ActivatedRasterOverlay>& activated :
                 activatedRasterOverlays_) {
                if (activated->getProjection() ==
                    RasterOverlayProjection::Gcj02WebMercator) {
                    ++env.overlayGcj02Count;
                }
            }
        }
        env.terrainGpuDisplacement = engine_.terrainGpuDisplacementEnabled();
        env.terrainPageStore = config_.terrainPageStore;
        env.decoupleImageryFromGeometry =
            config_.tileset.decoupleImageryFromGeometry;
        env.terrainFillProxy = config_.tileset.enableTerrainFillProxy;
        env.fxaa = fxaaOk && config_.fxaa;
        env.aerialFog = fogOk && config_.aerialFog;
        envsnap::logBoot(env);

        // 登记契约闸的实际状态。影像驱动的几何上采样(TexcoordNwOrigin 的判定点
        // 所在)只在 decoupleImageryFromGeometry=false 时存在;不登记的话它会永远
        // 报 dead,而常亮警告会训练出「这条可以忽略」的习惯。真机实测:decouple
        // =true → coverage 0;=false → 152 次求值零违约。
        contracts::setGateActive(
            contracts::Gate::ImageryDrivenUpsample,
            !config_.tileset.decoupleImageryFromGeometry);
    }
}

void EarthEngineSdkFacade::resetCamera() {
    const auto& ellipsoid = Ellipsoid::WGS84();
    auto targetEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromDegrees(config_.initialCamera.longitudeDegrees,
                                  config_.initialCamera.latitudeDegrees,
                                  0.0));

    // 斜视初始视角:自由视角(非 orbit),相机从目标点正南以给定仰角俯瞰,让竖直
    // foliage 等地表小物可见(nadir 下广告牌边缘朝相机≈隐形)。viewDistance 会把
    // orbitMode_ 置 false,故每帧不再被 orbit 重建覆盖。
    if (config_.initialCamera.obliqueElevationDegrees > 0.0) {
        const Vec3 upN = ellipsoid.geodeticSurfaceNormal(targetEcef);
        Vec3 eastN = Vec3::unitZ().cross(upN);
        if (eastN.lengthSquared() < 1e-12) {
            eastN = Vec3::unitX();
        }
        eastN = eastN.normalized();
        const Vec3 northN = upN.cross(eastN).normalized();
        const double elevRad =
            config_.initialCamera.obliqueElevationDegrees * M_PI / 180.0;
        const double dist =
            config_.initialCamera.heightMeters / std::max(0.05, std::sin(elevRad));
        // 相机在目标正南、抬高:eye = target + dist*(sin·up - cos·north)。
        const Vec3 eye = targetEcef +
            (upN * std::sin(elevRad) - northN * std::cos(elevRad)) * dist;
        engine_.camera().lookAt(eye, targetEcef, upN);
        engine_.cameraController().viewDistance(targetEcef, dist);
        // 位姿设定后再冻结，让 update() 停止一切扰动（测量台专用）。
        engine_.cameraController().setMeasurementFreeze(
            config_.initialCamera.freezeCamera);
        engine_.cameraController().setScriptedPan(
            config_.initialCamera.scriptedPan,
            config_.initialCamera.scriptedPanStartFrame,
            config_.initialCamera.scriptedPanFrames,
            config_.initialCamera.scriptedPanYawPerFrameRad);
        return;
    }
    auto camEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromDegrees(config_.initialCamera.longitudeDegrees,
                                  config_.initialCamera.latitudeDegrees,
                                  config_.initialCamera.heightMeters));
    Vec3 normal = ellipsoid.geodeticSurfaceNormal(targetEcef);
    // The camera sits directly overhead (same lon/lat as the target), so the
    // view direction is anti-parallel to the surface normal. Passing up=normal
    // makes lookAt degenerate: right = dir × up ≈ 0, and float cancellation of
    // the near-parallel cross product yields a near-collapsed camera basis that
    // smears the terrain into vertical streaks. Use local north as screen-up for
    // this nadir case (falls back to the ECEF X axis at the poles).
    Vec3 up = normal;
    Vec3 viewDir = (targetEcef - camEcef).normalized();
    const double align = viewDir.dot(normal);
    if (align * align > 0.998) {  // |cos| > ~0.999 → view ∥ normal
        Vec3 east = Vec3::unitZ().cross(normal);
        if (east.lengthSquared() < 1e-12) {
            east = Vec3::unitX();
        }
        up = normal.cross(east.normalized());
    }
    engine_.camera().lookAt(camEcef, targetEcef, up);

    // CameraController 以 orbit 模式每帧从自身 rotation_/distance_ 重建相机
    // （看向地心=nadir），否则上面的 lookAt 会在第 1 帧被覆盖、初始相机 config
    // 不生效。orbit 约定是 CameraController 的私有实现细节,这里只声明意图
    // "目标点正上方 heightMeters 高、正北朝上",推导收在控制器内。
    engine_.cameraController().setNadirOrbitView(
        targetEcef, normal, config_.initialCamera.heightMeters);
    // 位姿设定后再冻结（测量台专用）；nadir 冻结时 update() 跳过 orbit 重建，
    // 相机停在上面显式 lookAt 的正上方位姿。
    engine_.cameraController().setMeasurementFreeze(
        config_.initialCamera.freezeCamera);
    engine_.cameraController().setScriptedPan(
        config_.initialCamera.scriptedPan,
        config_.initialCamera.scriptedPanStartFrame,
        config_.initialCamera.scriptedPanFrames,
        config_.initialCamera.scriptedPanYawPerFrameRad);
}

void EarthEngineSdkFacade::addCustomImageryOverlay(
    std::unique_ptr<ImageryProvider> provider,
    std::unique_ptr<TileScheme> scheme,
    RasterOverlay::Options options) {
    pendingCustomOverlays_.push_back(
        PendingCustomOverlay{std::move(provider), std::move(scheme),
                             std::move(options)});
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
