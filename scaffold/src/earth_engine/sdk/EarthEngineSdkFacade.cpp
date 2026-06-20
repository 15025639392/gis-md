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
#include "../providers/QuantizedMeshTerrainProvider.h"
#include "../providers/XYZImageryProvider.h"
#include "../renderer/RenderDevice.h"
#include "../scene/Camera.h"
#include "../tiling/Tileset.h"

#include <memory>
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

std::unique_ptr<TerrainProvider> createTerrainProvider(
    const TerrainSourceConfig& config,
    PlatformBridge& platformBridge) {
    if (config.kind == TerrainSourceKind::None) {
        return {};
    }

    if (config.kind == TerrainSourceKind::QuantizedMesh) {
        auto qm = std::make_unique<QuantizedMeshTerrainProvider>(
            config.urlTemplate, config.attribution);
        qm->setZoomRange(config.minimumZoom, config.maximumZoom);
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
    hm->setZoomRange(config.minimumZoom, config.maximumZoom);
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

        auto xyz = std::make_unique<XYZImageryProvider>(
            overlayConfig.urlTemplate, overlayConfig.attribution);
        xyz->setZoomRange(overlayConfig.minimumZoom,
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
