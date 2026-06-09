#include "Engine.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "renderer/RenderDevice.h"
#include "layers/BasemapLayer.h"
#include "layers/VectorLayer.h"
#include "layers/TerrainLayer.h"
#include "interaction/InputEvent.h"
#include "interaction/PickingService.h"

#include <chrono>

namespace earth_engine {

Engine::Engine(RenderDevice* device)
    : device_(device),
      scene_(std::make_unique<Scene>()) {
}

Engine::~Engine() {
    onSurfaceDestroyed();
}

void Engine::onSurfaceCreated() {
    if (!device_) return;

    device_->onSurfaceCreated();
    scene_->setRenderDevice(device_);
    surfaceCreated_ = true;
}

void Engine::onSurfaceChanged(int widthPixels, int heightPixels, float dpr) {
    device_->onSurfaceChanged(widthPixels, heightPixels);
    scene_->setViewport(widthPixels, heightPixels, dpr);
}

void Engine::onSurfaceDestroyed() {
    scene_->setRenderDevice(nullptr);
    if (device_) {
        device_->onSurfaceDestroyed();
    }
    surfaceCreated_ = false;
}

void Engine::render(double deltaSeconds) {
    if (!surfaceCreated_ || !isReady()) return;

    // 自动计时
    if (deltaSeconds <= 0.0) {
        auto now = std::chrono::steady_clock::now();
        double nowSec = std::chrono::duration<double>(
            now.time_since_epoch()).count();
        if (lastRenderTime_ > 0.0) {
            deltaSeconds = nowSec - lastRenderTime_;
        } else {
            deltaSeconds = 1.0 / 60.0;
        }
        lastRenderTime_ = nowSec;
    }

    device_->beginFrame();
    scene_->update(deltaSeconds);
    scene_->render();
    device_->endFrame();
}

void Engine::onInputEvent(const InputEvent& event) {
    scene_->onInputEvent(event);
}

void Engine::onDragStart(float xPixels, float yPixels) {
    InputEvent event;
    event.type = InputEvent::Type::PointerDown;
    event.screenX = xPixels;
    event.screenY = yPixels;
    event.pointerType = InputEvent::PointerType::Touch;
    onInputEvent(event);
}

void Engine::onDragMove(float xPixels, float yPixels) {
    InputEvent event;
    event.type = InputEvent::Type::PointerMove;
    event.screenX = xPixels;
    event.screenY = yPixels;
    event.pointerType = InputEvent::PointerType::Touch;
    onInputEvent(event);
}

void Engine::onDragEnd() {
    InputEvent event;
    event.type = InputEvent::Type::PointerUp;
    event.pointerType = InputEvent::PointerType::Touch;
    onInputEvent(event);
}

Camera& Engine::camera() {
    return scene_->camera();
}

void Engine::addLayer(std::unique_ptr<BasemapLayer> layer) {
    scene_->addLayer(std::move(layer));
}

std::unique_ptr<BasemapLayer> Engine::removeLayer(const std::string& layerId) {
    return scene_->removeLayer(layerId);
}

void Engine::moveLayer(const std::string& layerId, size_t index) {
    scene_->moveLayer(layerId, index);
}

size_t Engine::layerCount() const {
    return scene_->layerCount();
}

int Engine::visibleTileCount() const {
    return scene_->visibleTileCount();
}

int Engine::cachedTileCount() const {
    return scene_->cachedTileCount();
}

std::vector<BasemapLayerStack::ControlPointResult>
Engine::verifyControlPoint(double lngRad, double latRad, int zoom) const {
    return scene_->verifyControlPoint(lngRad, latRad, zoom);
}

// ---- 矢量图层 ----

void Engine::addVectorLayer(std::unique_ptr<VectorLayer> layer) {
    scene_->addVectorLayer(std::move(layer));
}

std::unique_ptr<VectorLayer> Engine::removeVectorLayer(const std::string& layerId) {
    return scene_->removeVectorLayer(layerId);
}

size_t Engine::vectorLayerCount() const {
    return scene_->vectorLayerCount();
}

// ---- 地形 ----

void Engine::setTerrainLayer(std::unique_ptr<TerrainLayer> layer) {
    scene_->setTerrainLayer(std::move(layer));
}

void Engine::setTerrainEnabled(bool enabled) {
    scene_->setTerrainEnabled(enabled);
}

bool Engine::hasTerrain() const {
    return scene_->hasTerrain();
}

// ---- 拾取与选择 ----

PickResult Engine::pick(float screenX, float screenY) const {
    return scene_->pick(screenX, screenY);
}

void Engine::onHover(const PickResult& result) {
    scene_->onHover(result);
}

void Engine::onSelect(const PickResult& result) {
    scene_->onSelect(result);
}

void Engine::clearSelection() {
    scene_->clearSelection();
}

// ---- 环境系统 ----

void Engine::setTime(double julianDate) {
    scene_->setTime(julianDate);
}

double Engine::time() const {
    return scene_->time();
}

void Engine::advanceTime(double seconds) {
    scene_->advanceTime(seconds);
}

Vec3 Engine::sunDirection() const {
    return scene_->sunDirection();
}

void Engine::getClearColor(float& r, float& g, float& b, float& a) const {
    const auto& fs = scene_->frameState();
    r = fs.clearR;
    g = fs.clearG;
    b = fs.clearB;
    a = fs.clearA;
}

void Engine::setDebugOverlayEnabled(bool enabled) {
    scene_->setDebugOverlayEnabled(enabled);
}

bool Engine::debugOverlayEnabled() const {
    return scene_->debugOverlayEnabled();
}

void Engine::setNormalMapDebugEnabled(bool enabled) {
    // Forward to all basemap layers through the Scene's layer stack.
    auto& stack = scene_->layerStack();
    for (auto& layer : stack.layers()) {
        layer->setNormalMapDebugEnabled(enabled);
    }
}

const Diagnostics& Engine::diagnostics() const {
    return scene_->diagnostics();
}

bool Engine::isReady() const {
    return scene_ && scene_->isReady();
}

} // namespace earth_engine
