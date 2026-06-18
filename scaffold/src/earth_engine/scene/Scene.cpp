#include "Scene.h"
#include "Camera.h"
#include "SceneRenderPipeline.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Transforms.h"
#include "../debug/PerfTimer.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cstdio>
#include <utility>
#include <cmath>

namespace earth_engine {
namespace {

std::array<double, 3> toArray(const Vec3& v) {
    return {v.x(), v.y(), v.z()};
}

std::array<double, 16> toArray(const Mat4& m) {
    std::array<double, 16> values{};
    const double* raw = glm::value_ptr(m.raw());
    std::copy(raw, raw + values.size(), values.begin());
    return values;
}

double wrapPositiveRadians(double radians) {
    constexpr double kTwoPi = glm::two_pi<double>();
    double wrapped = std::fmod(radians, kTwoPi);
    if (wrapped < 0.0) {
        wrapped += kTwoPi;
    }
    return wrapped;
}

} // namespace

Scene::Scene()
    : camera_(std::make_unique<Camera>()),
      cameraController_(std::make_unique<CameraController>(camera_.get())),
      inputManager_(std::make_unique<InputManager>()),
      pickingService_(std::make_unique<PickingService>()),
      selectionManager_(std::make_unique<SelectionManager>()),
      timeController_(std::make_unique<TimeController>()),
      skyGradient_(std::make_unique<SkyGradient>()),
      atmospherePass_(std::make_unique<AtmosphereBackgroundPass>()),
      skyBox_(std::make_unique<SkyBox>()) {

    // OpenGlobus PlanetCamera reverse-Z defaults: near=150, far=1e12.
    camera_->setPerspective(
        camera_->verticalFovRadians(),
        150.0,
        1e12);
    // Depth func/clear are configured per-platform by RenderDevice.
    globeMesh_ = Globe::createMesh(96, 48);
    configureCameraSurfacePicker();
    setupSelectionCallbacks();
    setupInputCallback();
}

Scene::~Scene() {
    renderer_.reset();
}

bool Scene::setRenderDevice(RenderDevice* device) {
    renderDevice_ = device;
    if (!device) {
        renderPipeline_.reset();
        renderer_.reset();
        return false;
    }

    renderer_ = std::make_unique<Renderer>(device);
    renderPipeline_ = std::make_unique<SceneRenderPipeline>();
    if (!renderer_->initialize(globeMesh_)) {
        fprintf(stderr, "[Scene] renderer_->initialize() FAILED\n");
        renderPipeline_.reset();
        return false;
    }

    // 初始化环境系统渲染 Pass
    atmospherePass_->initialize(device);
    skyBox_->initialize(device);

    return true;
}

void Scene::setViewport(int widthPixels, int heightPixels, float dpr) {
    frameState_.viewportWidthPixels = widthPixels;
    frameState_.viewportHeightPixels = heightPixels;
    frameState_.devicePixelRatio = dpr;

    if (cameraController_) {
        cameraController_->setViewport(widthPixels, heightPixels);
    }
}

void Scene::update(double deltaSeconds) {
    const double updateStartMs = perf::nowMs();

    frameState_.diagnostics.cameraUpdateMs = 0.0;
    frameState_.diagnostics.environmentUpdateMs = 0.0;
    frameState_.diagnostics.basemapStackUpdateMs = 0.0;
    frameState_.diagnostics.terrainUpdateMs = 0.0;
    frameState_.diagnostics.contentTilesetUpdateMs = 0.0;
    frameState_.diagnostics.renderCommandBuildMs = 0.0;
    frameState_.diagnostics.renderSubmitMs = 0.0;

    if (cameraController_) {
        const double startMs = perf::nowMs();
        cameraController_->update(deltaSeconds);
        frameState_.diagnostics.cameraUpdateMs = perf::nowMs() - startMs;
    }

    elapsedTime_ += deltaSeconds;
    frameState_.frameId = ++frameId_;
    frameState_.timeSeconds = elapsedTime_;
    frameState_.deltaSeconds = deltaSeconds;
    frameState_.camera = camera_.get();
    populateSelectorViews();
    constexpr double kInteractionFocusTtlSeconds = 2.5;
    frameState_.hasInteractionFocus =
        hasInteractionFocus_ &&
        interactionFocusTimeSeconds_ >= 0.0 &&
        elapsedTime_ - interactionFocusTimeSeconds_ <= kInteractionFocusTtlSeconds;
    frameState_.interactionFocusDirection = frameState_.hasInteractionFocus
        ? interactionFocusDirection_
        : Vec3::zero();

    // 更新 FPS（5 帧平滑）
    if (deltaSeconds > 0.0) {
        frameState_.diagnostics.frameTimeMs = deltaSeconds * 1000.0;
        constexpr double kFpsSmoothing = 0.1;
        frameState_.diagnostics.fps =
            frameState_.diagnostics.fps * (1.0 - kFpsSmoothing) +
            (1.0 / deltaSeconds) * kFpsSmoothing;
    }

    {
        const double startMs = perf::nowMs();
        Vec3 sunDir = SunDirection::compute(timeController_->julianDate());
        double camAlt = camera_->getHeight();
        Vec3 localUp = Ellipsoid::WGS84().geodeticSurfaceNormal(camera_->position());
        skyGradient_->update(sunDir, localUp, camAlt);
        frameState_.lightDir = {
            static_cast<float>(sunDir.x()),
            static_cast<float>(sunDir.y()),
            static_cast<float>(sunDir.z())
        };
        auto& hc = skyGradient_->horizonColor();
        frameState_.clearR = hc[0];
        frameState_.clearG = hc[1];
        frameState_.clearB = hc[2];
        frameState_.diagnostics.environmentUpdateMs = perf::nowMs() - startMs;
    }

    // 统一 Tileset 更新（cesium-native 对齐）
    if (tileset_) {
        const double startMs = perf::nowMs();
        tileset_->update(frameState_);
        frameState_.diagnostics.terrainUpdateMs = perf::nowMs() - startMs;
    }
    if (!additionalTilesets_.empty()) {
        const double startMs = perf::nowMs();
        for (auto& tileset : additionalTilesets_) {
            if (tileset) {
                tileset->update(frameState_);
            }
        }
        frameState_.diagnostics.contentTilesetUpdateMs =
            perf::nowMs() - startMs;
    }

    char detail[192];
    std::snprintf(detail, sizeof(detail),
        "camera=%.2f env=%.2f basemap=%.2f terrain=%.2f content=%.2f",
        frameState_.diagnostics.cameraUpdateMs,
        frameState_.diagnostics.environmentUpdateMs,
        frameState_.diagnostics.basemapStackUpdateMs,
        frameState_.diagnostics.terrainUpdateMs,
        frameState_.diagnostics.contentTilesetUpdateMs);
    perf::logTiming(frameState_.frameId,
                    "Scene.update.total",
                    perf::nowMs() - updateStartMs,
                    detail);
}

void Scene::setSelectorViewOverride(
    std::vector<FrameState::SelectorView> selectorViews) {
    hasSelectorViewOverride_ = true;
    selectorViewOverride_ = std::move(selectorViews);
}

void Scene::clearSelectorViewOverride() {
    hasSelectorViewOverride_ = false;
    selectorViewOverride_.clear();
}

void Scene::setOcclusionCallback(Tileset::OcclusionCallback callback) {
    occlusionCallback_ = std::move(callback);
    if (tileset_) {
        tileset_->setOcclusionCallback(occlusionCallback_);
    }
    for (auto& tileset : additionalTilesets_) {
        if (tileset) {
            tileset->setOcclusionCallback(occlusionCallback_);
        }
    }
}

void Scene::clearOcclusionCallback() {
    occlusionCallback_ = nullptr;
    if (tileset_) {
        tileset_->clearOcclusionCallback();
    }
    for (auto& tileset : additionalTilesets_) {
        if (tileset) {
            tileset->clearOcclusionCallback();
        }
    }
}

void Scene::populateSelectorViews() {
    frameState_.selectorViews.clear();
    if (hasSelectorViewOverride_) {
        frameState_.selectorViews = selectorViewOverride_;
        return;
    }

    if (!frameState_.camera) {
        return;
    }

    FrameState::SelectorView selectorView;
    selectorView.position = frameState_.camera->position();
    selectorView.direction = frameState_.camera->direction();
    const double viewportWidth =
        static_cast<double>(frameState_.viewportWidthPixels);
    const double viewportHeight =
        static_cast<double>(frameState_.viewportHeightPixels);
    selectorView.projectionMatrix =
        frameState_.camera->projectionMatrix(viewportWidth, viewportHeight);
    selectorView.frustum = Frustum::fromViewProjection(
        selectorView.projectionMatrix * frameState_.camera->viewMatrix());
    selectorView.viewportHeightPixels = frameState_.viewportHeightPixels;
    frameState_.selectorViews.push_back(selectorView);
}

void Scene::render() {
    if (!renderer_ || !renderPipeline_ || !isReady()) return;

    renderPipeline_->render(SceneRenderPipeline::Context{
        frameState_,
        *renderer_,
        renderCommands_,
        skyBox_.get(),
        atmospherePass_.get(),
        skyGradient_.get(),
        tileset_.get(),
        additionalTilesets_,
        vectorLayers_,
        [this]() { updatePresentationTrace(); }});
}

void Scene::updatePresentationTrace() {
    PresentationTrace trace;

    trace.camera.frameId = frameState_.frameId;
    trace.camera.viewportWidthPixels = frameState_.viewportWidthPixels;
    trace.camera.viewportHeightPixels = frameState_.viewportHeightPixels;
    trace.camera.devicePixelRatio = frameState_.devicePixelRatio;

    if (frameState_.camera) {
        const Camera& cam = *frameState_.camera;
        const Vec3 target = cam.target();
        Cartographic targetCartographic =
            std::isnan(target.x())
                ? Ellipsoid::WGS84().cartesianToCartographic(cam.position())
                : Ellipsoid::WGS84().cartesianToCartographic(target);
        const Vec3 localUp =
            Ellipsoid::WGS84().geodeticSurfaceNormal(targetCartographic);
        const Mat4 enuToEcef =
            Transforms::eastNorthUpToFixedFrame(
                Ellipsoid::WGS84().cartographicToCartesian(targetCartographic));
        const glm::dmat4 ecefToEnu = glm::inverse(enuToEcef.raw());
        const glm::dvec4 localDirection4 =
            ecefToEnu * glm::dvec4(cam.direction().raw(), 0.0);
        const glm::dvec3 localDirection =
            glm::normalize(glm::dvec3(localDirection4));

        trace.camera.verticalFovRadians = cam.verticalFovRadians();
        trace.camera.targetLongitudeDegrees =
            targetCartographic.longitudeDegrees();
        trace.camera.targetLatitudeDegrees =
            targetCartographic.latitudeDegrees();
        trace.camera.targetHeightMeters = targetCartographic.height();
        trace.camera.cameraHeightMeters = cam.getHeight();
        trace.camera.pitchRadians =
            std::asin(std::clamp(localDirection.z, -1.0, 1.0));
        trace.camera.headingRadians =
            wrapPositiveRadians(std::atan2(localDirection.x, localDirection.y));
        trace.camera.position = toArray(cam.position());
        trace.camera.direction = toArray(cam.direction());
        trace.camera.up = toArray(cam.up());
        trace.camera.right = toArray(cam.right());
        (void)localUp;
    }

    trace.selectorViews.reserve(frameState_.selectorViews.size());
    for (const FrameState::SelectorView& view : frameState_.selectorViews) {
        PresentationSelectorViewTrace viewTrace;
        viewTrace.position = toArray(view.position);
        viewTrace.direction = toArray(view.direction);
        viewTrace.viewportHeightPixels = view.viewportHeightPixels;
        viewTrace.projectionMatrix = toArray(view.projectionMatrix);
        trace.selectorViews.push_back(viewTrace);
    }

    auto appendTileset = [&](const Tileset* tileset) {
        if (!tileset) return;
        const TilePlan& plan = tileset->tilePlan();
        PresentationTilesetTrace tilesetTrace;
        tilesetTrace.visibleTiles = plan.visibleTiles;
        tilesetTrace.minVisibleZoom = plan.minVisibleZoom;
        tilesetTrace.maxVisibleZoom = plan.maxVisibleZoom;
        tilesetTrace.lodSizePixels = plan.lodSizePixels;
        tilesetTrace.renderEntries.reserve(plan.renderEntries.size());
        for (const TileRenderEntry& entry : plan.renderEntries) {
            PresentationRenderEntryTrace entryTrace;
            entryTrace.selectedKey = entry.selectedKey;
            entryTrace.renderKey = entry.renderKey;
            entryTrace.opacity = entry.opacity;
            entryTrace.selectedThisFrame = entry.selectedThisFrame;
            entryTrace.usesAncestorFallback = entry.usesAncestorFallback;
            entryTrace.allowSynchronousMeshPrep =
                entry.allowSynchronousMeshPrep;
            entryTrace.surfaceClipEnabled = entry.surfaceClipEnabled;
            entryTrace.surfaceClipUv = entry.surfaceClipUv;
            tilesetTrace.renderEntries.push_back(entryTrace);
        }
        trace.tilesets.push_back(std::move(tilesetTrace));
    };

    appendTileset(tileset_.get());
    for (const auto& tileset : additionalTilesets_) {
        appendTileset(tileset.get());
    }

    trace.commands.reserve(renderCommands_.size());
    for (const RenderCommand& command : renderCommands_) {
        PresentationCommandTrace commandTrace;
        commandTrace.kind = command.kind;
        commandTrace.owner = command.owner;
        commandTrace.surfaceGeometryZoom = command.surfaceGeometryZoom;
        commandTrace.surfaceTextureZoom = command.surfaceTextureZoom;
        commandTrace.indexOffset = command.indexOffset;
        commandTrace.indexCount = command.indexCount;
        commandTrace.surfaceMeshIndexCount = command.surfaceMeshIndexCount;
        commandTrace.surfaceNoSkirtIndexCount =
            command.surfaceNoSkirtIndexCount;
        commandTrace.surfaceSkirtIndexCount = command.surfaceSkirtIndexCount;
        commandTrace.surfaceBaseRasterState = command.surfaceBaseRasterState;
        commandTrace.surfaceBaseIsRectangleTile =
            command.surfaceBaseIsRectangleTile;
        commandTrace.surfaceOverlayTextureCount =
            command.surfaceOverlayTextureCount;
        commandTrace.surfaceClipEnabled = command.surfaceClipEnabled;
        commandTrace.surfaceClipUv = command.surfaceClipUv;
        commandTrace.surfaceTransitionOpacity =
            command.surfaceTransitionOpacity;
        commandTrace.frameId = command.frameId;
        commandTrace.generation = command.generation;
        trace.commands.push_back(std::move(commandTrace));
    }

    presentationTrace_ = std::move(trace);
}

void Scene::setTileset(std::unique_ptr<Tileset> tileset) {
    tileset_ = std::move(tileset);
    if (tileset_) {
        if (occlusionCallback_) {
            tileset_->setOcclusionCallback(occlusionCallback_);
        } else {
            tileset_->clearOcclusionCallback();
        }
    }
    configureCameraSurfacePicker();
}

void Scene::addTileset(std::unique_ptr<Tileset> tileset) {
    if (!tileset) return;
    if (occlusionCallback_) {
        tileset->setOcclusionCallback(occlusionCallback_);
    }
    additionalTilesets_.push_back(std::move(tileset));
}

// ---- 矢量图层管理 ----

void Scene::addVectorLayer(std::unique_ptr<VectorLayer> layer) {
    if (!layer) return;
    layer->initialize(renderDevice_);
    vectorLayers_.push_back(std::move(layer));
}

std::unique_ptr<VectorLayer> Scene::removeVectorLayer(const std::string& layerId) {
    auto it = std::find_if(vectorLayers_.begin(), vectorLayers_.end(),
        [&](const auto& l) { return l->id() == layerId; });
    if (it == vectorLayers_.end()) return nullptr;

    auto removed = std::move(*it);
    vectorLayers_.erase(it);
    return removed;
}

// ---- 拾取与选择 ----

PickResult Scene::pick(float screenX, float screenY) const {
    if (!pickingService_ || !camera_) return PickResult{};

    std::function<float(double,double)> terrainSampler;
    if (tileset_) {
        terrainSampler = [this](double lng, double lat) {
            return tileset_->sampleHeight(lng, lat);
        };
    }

    // 先做地形拾取
    PickResult result = pickingService_->pickTerrain(
        screenX, screenY,
        *camera_,
        static_cast<double>(frameState_.viewportWidthPixels),
        static_cast<double>(frameState_.viewportHeightPixels),
        terrainSampler);

    // 再检查矢量图层（替换更近的命中）
    std::vector<const VectorLayer*> layerPtrs;
    for (const auto& l : vectorLayers_) {
        layerPtrs.push_back(l.get());
    }

    auto vecResult = pickingService_->pick(
        screenX, screenY,
        *camera_,
        static_cast<double>(frameState_.viewportWidthPixels),
        static_cast<double>(frameState_.viewportHeightPixels),
        layerPtrs);

    if (vecResult.hitType == PickResult::HitType::VectorFeature &&
        (!result.isValid() || vecResult.distance < result.distance)) {
        result = vecResult;
    }

    return result;
}

void Scene::onHover(const PickResult& result) {
    if (selectionManager_) {
        selectionManager_->onHover(result);
    }
}

void Scene::onSelect(const PickResult& result) {
    if (selectionManager_) {
        selectionManager_->onSelect(result);
    }
}

void Scene::clearSelection() {
    if (selectionManager_) {
        selectionManager_->clearSelection();
    }
}

// ---- 选择回调 ----

void Scene::setupSelectionCallbacks() {
    selectionManager_->setStateChangeCallback(
        [this](const std::string& layerId,
               const std::string& featureId,
               FeatureState state) {
            // 转换 FeatureState → VectorLayer 可理解的字符串
            const char* stateStr = "";
            switch (state) {
                case FeatureState::Hovered:  stateStr = "hover"; break;
                case FeatureState::Selected: stateStr = "selected"; break;
                default: break;
            }
            for (auto& layer : vectorLayers_) {
                if (layer->id() == layerId) {
                    layer->setFeatureState(featureId, stateStr);
                    return;
                }
            }
        });
}

// ---- 输入回调 ----

void Scene::configureCameraSurfacePicker() {
    if (!cameraController_) return;

    cameraController_->setSurfacePicker(
        [this](float screenX, float screenY, Vec3& outPoint) {
            if (!pickingService_ || !camera_) {
                return false;
            }

            PickResult result = pickingService_->pickTerrain(
                screenX, screenY,
                *camera_,
                static_cast<double>(frameState_.viewportWidthPixels),
                static_cast<double>(frameState_.viewportHeightPixels),
                tileset_
                    ? std::function<float(double,double)>(
                          [this](double lng, double lat) {
                              return tileset_->sampleHeight(lng, lat);
                          })
                    : std::function<float(double,double)>{});
            if (!result.isValid()) {
                return false;
            }

            outPoint = result.worldPosition;
            return true;
        });

    cameraController_->setTerrainHeightFunc(
        tileset_
            ? CameraController::TerrainHeightFunc(
                  [this](const Vec3& ecefPosition) -> double {
                      const Cartographic c =
                          Ellipsoid::WGS84().cartesianToCartographic(ecefPosition);
                      return static_cast<double>(
                          tileset_->sampleHeight(c.longitude(), c.latitude()));
                  })
            : CameraController::TerrainHeightFunc{});
}

void Scene::setupInputCallback() {
    inputManager_->setCallback(
        [this](InputManager::Gesture gesture, const InputEvent& event) {
            switch (gesture) {
                case InputManager::Gesture::DragStart:
                    cameraController_->onDragStart(event.screenX, event.screenY,
                                                   event.timestamp);
                    break;
                case InputManager::Gesture::DragMove:
                    cameraController_->onDragMove(event.screenX, event.screenY,
                                                  event.timestamp);
                    break;
                case InputManager::Gesture::DragEnd:
                    cameraController_->onDragEnd();
                    break;
                case InputManager::Gesture::PinchStart:
                    cameraController_->onPinchGesture(event.pinchScale,
                                                      event.screenX,
                                                      event.screenY,
                                                      event.rotationRadians,
                                                      event.centerDeltaX,
                                                      event.centerDeltaY,
                                                      event.timestamp);
                    break;
                case InputManager::Gesture::PinchMove:
                    cameraController_->onPinchGesture(event.pinchScale,
                                                      event.screenX,
                                                      event.screenY,
                                                      event.rotationRadians,
                                                      event.centerDeltaX,
                                                      event.centerDeltaY,
                                                      event.timestamp);
                    break;
                case InputManager::Gesture::PinchEnd:
                    cameraController_->onPinchEnd();
                    break;
                case InputManager::Gesture::Click:
                case InputManager::Gesture::DoubleClick: {
                    PickResult result = pick(event.screenX, event.screenY);
                    if (gesture == InputManager::Gesture::DoubleClick) {
                        if (result.isValid()) {
                            cameraController_->viewDistance(
                                result.worldPosition,
                                result.distance * 0.57);
                        } else {
                            // 以点击位置为中心放大（缩小距离 30%）
                            float newDist = cameraController_->distance() * 0.7f;
                            cameraController_->setDistance(newDist);
                        }
                    } else {
                        // 单击：带修饰键的选择逻辑
                        if (result.isValid()) {
                            if (event.modifiers.shift) {
                                selectionManager_->onSelectAdd(result);
                            } else if (event.modifiers.ctrl || event.modifiers.meta) {
                                selectionManager_->onSelectToggle(result);
                            } else {
                                selectionManager_->onSelect(result);
                            }
                        } else {
                            clearSelection();
                        }
                    }
                    break;
                }
            }
        });
}

bool Scene::pickInteractionFocus(float screenX, float screenY, Vec3& outPoint) const {
    if (!pickingService_ || !camera_) {
        return false;
    }

    const PickResult result = pickingService_->pickTerrain(
        screenX,
        screenY,
        *camera_,
        static_cast<double>(frameState_.viewportWidthPixels),
        static_cast<double>(frameState_.viewportHeightPixels),
        tileset_
            ? std::function<float(double,double)>(
                  [this](double lng, double lat) {
                      return tileset_->sampleHeight(lng, lat);
                  })
            : std::function<float(double,double)>{});
    if (!result.isValid()) {
        return false;
    }

    outPoint = result.worldPosition;
    return true;
}

void Scene::updateInteractionFocus(const InputEvent& event) {
    switch (event.type) {
        case InputEvent::Type::PointerDown:
        case InputEvent::Type::PointerMove:
        case InputEvent::Type::PointerUp:
        case InputEvent::Type::PinchStart:
        case InputEvent::Type::PinchMove:
        case InputEvent::Type::PinchEnd:
            break;
        default:
            return;
    }

    Vec3 focusPoint;
    if (!pickInteractionFocus(event.screenX, event.screenY, focusPoint)) {
        return;
    }
    interactionFocusDirection_ = focusPoint.normalized();
    interactionFocusTimeSeconds_ = elapsedTime_;
    hasInteractionFocus_ = true;
}

void Scene::onInputEvent(const InputEvent& event) {
    updateInteractionFocus(event);
    if (inputManager_) {
        inputManager_->process(event);
    }
}

// ---- 环境系统 ----

void Scene::setTime(double julianDate) {
    timeController_->setJulianDate(julianDate);
}

double Scene::time() const {
    return timeController_->julianDate();
}

void Scene::advanceTime(double seconds) {
    timeController_->advanceSeconds(seconds);
}

Vec3 Scene::sunDirection() const {
    return SunDirection::compute(timeController_->julianDate());
}

} // namespace earth_engine
