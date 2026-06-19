#include "Engine.h"
#include "scene/Scene.h"
#include "tiling/Tileset.h"
#include "scene/Camera.h"
#include "renderer/RenderDevice.h"
#include "layers/VectorLayer.h"
#include "interaction/InputEvent.h"
#include "interaction/PickingService.h"
#include "debug/PerfTimer.h"

#include <chrono>
#include <cstdio>
#include <utility>

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
    if (!scene_->setRenderDevice(device_)) {
        surfaceCreated_ = false;
        return;
    }
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
    if (!surfaceCreated_ || !isReady()) { fprintf(stderr, "[Engine::render] BLOCKED: surface=%d ready=%d\n", surfaceCreated_, isReady()); return; }
    const double frameStartMs = perf::nowMs();

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

    {
        const double startMs = perf::nowMs();
        device_->beginFrame();
        scene_->recordEngineTiming(
            Scene::EngineTimingScope::BeginFrame,
            perf::nowMs() - startMs);
    }
    {
        const double startMs = perf::nowMs();
        scene_->update(deltaSeconds);
        scene_->recordEngineTiming(
            Scene::EngineTimingScope::SceneUpdate,
            perf::nowMs() - startMs);
    }
    {
        const double startMs = perf::nowMs();
        scene_->render();
        scene_->recordEngineTiming(
            Scene::EngineTimingScope::SceneRender,
            perf::nowMs() - startMs);
    }
    {
        const double startMs = perf::nowMs();
        device_->endFrame();
        scene_->recordEngineTiming(
            Scene::EngineTimingScope::EndFrame,
            perf::nowMs() - startMs);
    }

    scene_->finishEngineFrame(perf::nowMs() - frameStartMs);
    const Diagnostics& diag = scene_->diagnostics();
    char detail[256];
    std::snprintf(detail, sizeof(detail),
        "begin=%.2f update=%.2f render=%.2f submit=%.2f end=%.2f draw=%d tiles=%d",
        diag.engineBeginFrameMs,
        diag.sceneUpdateMs,
        diag.sceneRenderMs,
        diag.renderSubmitMs,
        diag.engineEndFrameMs,
        diag.drawCalls,
        diag.visibleTiles);
    perf::logTiming(scene_->frameState().frameId,
                    "Engine.render.total",
                    diag.engineFrameCpuMs,
                    detail);
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

void Engine::setTileset(std::unique_ptr<Tileset> tileset) {
    scene_->setTileset(std::move(tileset));
}

void Engine::addTileset(std::unique_ptr<Tileset> tileset) {
    scene_->addTileset(std::move(tileset));
}

void Engine::setSelectorViewOverride(
    std::vector<FrameState::SelectorView> selectorViews) {
    scene_->setSelectorViewOverride(std::move(selectorViews));
}

void Engine::clearSelectorViewOverride() {
    scene_->clearSelectorViewOverride();
}

void Engine::setOcclusionCallback(TileOcclusionCallback callback) {
    scene_->setOcclusionCallback(std::move(callback));
}

void Engine::clearOcclusionCallback() {
    scene_->clearOcclusionCallback();
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

const Diagnostics& Engine::diagnostics() const {
    return scene_->diagnostics();
}

const PresentationTrace& Engine::presentationTrace() const {
    return scene_->presentationTrace();
}

bool Engine::isReady() const {
    return scene_ && scene_->isReady();
}

} // namespace earth_engine
