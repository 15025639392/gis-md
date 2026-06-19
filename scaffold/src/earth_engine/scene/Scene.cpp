#include "Scene.h"
#include "Camera.h"
#include "SceneEnvironmentCoordinator.h"
#include "SceneInteractionCoordinator.h"
#include "SceneLayerCoordinator.h"
#include "SceneFrameUpdateCoordinator.h"
#include "ScenePresentationTraceBuilder.h"
#include "SceneRenderPipeline.h"
#include "SceneTelemetryCoordinator.h"
#include "SceneTilesetCoordinator.h"
#include "../camera/CameraController.h"
#include "../environment/SkyGradient.h"
#include "../globe/Globe.h"
#include "../interaction/InputEvent.h"
#include "../layers/VectorLayer.h"
#include "../renderer/Renderer.h"
#include "../tiling/Tileset.h"

#include <utility>

namespace earth_engine {

Scene::Scene()
    : camera_(std::make_unique<Camera>()),
      cameraController_(std::make_unique<CameraController>(camera_.get())),
      globeMesh_(std::make_unique<GlobeMesh>(Globe::createMesh(96, 48))),
      layers_(std::make_unique<SceneLayerCoordinator>()),
      tilesets_(std::make_unique<SceneTilesetCoordinator>()),
      interaction_(std::make_unique<SceneInteractionCoordinator>()),
      environment_(std::make_unique<SceneEnvironmentCoordinator>()),
      telemetry_(std::make_unique<SceneTelemetryCoordinator>()) {

    // OpenGlobus PlanetCamera reverse-Z defaults: near=150, far=1e12.
    camera_->setPerspective(
        camera_->verticalFovRadians(),
        150.0,
        1e12);
    // Depth func/clear are configured per-platform by RenderDevice.
    configureCameraSurfacePicker();
    interaction_->setFeatureStateChangeCallback(
        [this](const std::string& layerId,
               const std::string& featureId,
               FeatureState state) {
            layers_->applyFeatureState(layerId, featureId, state);
        });
}

Scene::~Scene() {
    renderer_.reset();
}

bool Scene::setRenderDevice(RenderDevice* device) {
    renderDevice_ = device;
    layers_->setRenderDevice(device);
    if (!device) {
        renderPipeline_.reset();
        renderer_.reset();
        return false;
    }

    renderer_ = std::make_unique<Renderer>(device);
    renderPipeline_ = std::make_unique<SceneRenderPipeline>();
    if (!renderer_->initialize(*globeMesh_)) {
        fprintf(stderr, "[Scene] renderer_->initialize() FAILED\n");
        renderPipeline_.reset();
        return false;
    }

    environment_->initializeRenderResources(device);

    return true;
}

void Scene::setViewport(int widthPixels, int heightPixels, float dpr) {
    frameRuntime_.setViewport(widthPixels, heightPixels, dpr);

    if (cameraController_) {
        cameraController_->setViewport(widthPixels, heightPixels);
    }
}

void Scene::update(double deltaSeconds) {
    SceneFrameUpdateCoordinator::update(
        frameRuntime_.makeFrameUpdateInput(
            telemetry_->diagnostics(),
            camera_.get(),
            cameraController_.get(),
            *tilesets_,
            deltaSeconds,
            interaction_->hasInteractionFocus(),
            interaction_->interactionFocusDirection(),
            interaction_->interactionFocusTimeSeconds(),
            environment_->timeController(),
            environment_->skyGradient()));
}

void Scene::setSelectorViewOverride(
    std::vector<SelectorView> selectorViews) {
    frameRuntime_.setSelectorViewOverride(std::move(selectorViews));
}

void Scene::clearSelectorViewOverride() {
    frameRuntime_.clearSelectorViewOverride();
}

void Scene::setOcclusionCallback(TileOcclusionCallback callback) {
    tilesets_->setOcclusionCallback(std::move(callback));
}

void Scene::clearOcclusionCallback() {
    tilesets_->clearOcclusionCallback();
}

const Diagnostics& Scene::diagnostics() const {
    return telemetry_->diagnostics();
}

void Scene::recordEngineTiming(
    EngineTimingScope scope,
    double elapsedMs) {
    telemetry_->recordEngineTiming(scope, elapsedMs);
}

void Scene::finishEngineFrame(double elapsedMs) {
    telemetry_->finishEngineFrame(elapsedMs);
}

const PresentationTrace& Scene::presentationTrace() const {
    return telemetry_->presentationTrace();
}

void Scene::render() {
    if (!renderer_ || !renderPipeline_ || !isReady()) return;

    SceneRenderPipeline::Result renderResult =
        renderPipeline_->render(SceneRenderPipeline::Context{
        frameRuntime_.frameState(),
        telemetry_->diagnostics(),
        *renderer_,
        frameRuntime_.renderCommands(),
        environment_->skyBox(),
        environment_->atmospherePass(),
        environment_->skyGradient(),
        tilesets_->primary(),
        tilesets_->contentTilesets(),
        layers_->vectorLayers(),
        [this]() { updatePresentationTrace(); }});
    telemetry_->replaceRenderDiagnostics(renderResult.diagnostics);
}

void Scene::updatePresentationTrace() {
    telemetry_->updatePresentationTrace(
        frameRuntime_.makePresentationTraceInput(
            tilesets_->primary(),
            tilesets_->contentTilesets()));
}

void Scene::setTileset(std::unique_ptr<Tileset> tileset) {
    tilesets_->setPrimary(std::move(tileset));
    configureCameraSurfacePicker();
}

void Scene::addTileset(std::unique_ptr<Tileset> tileset) {
    tilesets_->addContent(std::move(tileset));
}

Tileset* Scene::tileset() const {
    return tilesets_->primary();
}

size_t Scene::additionalTilesetCount() const {
    return tilesets_->contentTilesetCount();
}

bool Scene::hasTerrain() const {
    return tilesets_->hasPrimaryTerrain();
}

// ---- 矢量图层管理 ----

void Scene::addVectorLayer(std::unique_ptr<VectorLayer> layer) {
    layers_->addVectorLayer(std::move(layer));
}

std::unique_ptr<VectorLayer> Scene::removeVectorLayer(const std::string& layerId) {
    return layers_->removeVectorLayer(layerId);
}

size_t Scene::vectorLayerCount() const {
    return layers_->vectorLayerCount();
}

// ---- 拾取与选择 ----

PickResult Scene::pick(float screenX, float screenY) const {
    return interaction_->pick(interactionContext(), screenX, screenY);
}

void Scene::onHover(const PickResult& result) {
    interaction_->onHover(result);
}

void Scene::onSelect(const PickResult& result) {
    interaction_->onSelect(result);
}

void Scene::clearSelection() {
    interaction_->clearSelection();
}

// ---- 输入回调 ----

void Scene::configureCameraSurfacePicker() {
    if (!cameraController_) return;

    interaction_->configureCameraSurfacePicker(
        *cameraController_,
        [this]() { return interactionContext(); });
}

SceneInteractionContext Scene::interactionContext() const {
    return frameRuntime_.makeInteractionContext(
        camera_.get(),
        cameraController_.get(),
        tilesets_->primary(),
        &layers_->vectorLayers());
}

void Scene::onInputEvent(const InputEvent& event) {
    interaction_->onInputEvent(interactionContext(), event);
}

// ---- 环境系统 ----

void Scene::setTime(double julianDate) {
    environment_->setTime(julianDate);
}

double Scene::time() const {
    return environment_->time();
}

void Scene::advanceTime(double seconds) {
    environment_->advanceTime(seconds);
}

Vec3 Scene::sunDirection() const {
    return environment_->sunDirection();
}

const SkyGradient& Scene::skyGradient() const {
    return *environment_->skyGradient();
}

} // namespace earth_engine
