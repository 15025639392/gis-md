#include "Scene.h"
#include "Camera.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Cartographic.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <limits>

namespace earth_engine {
namespace {

void updateSurfaceCommandDiagnostics(const RenderCommandList& commands,
                                     uint64_t expectedFrameId,
                                     Diagnostics& diag) {
    diag.staleSurfaceCommands = 0;
    diag.missingGenerationSurfaceCommands = 0;
    diag.minSurfaceGeneration = 0;
    diag.maxSurfaceGeneration = 0;

    uint64_t minGeneration = std::numeric_limits<uint64_t>::max();
    uint64_t maxGeneration = 0;
    bool sawGeneration = false;

    for (const RenderCommand& cmd : commands) {
        if (cmd.kind != RenderCommandKind::SurfaceTile) continue;

        if (expectedFrameId != 0 && cmd.frameId != expectedFrameId) {
            ++diag.staleSurfaceCommands;
        }
        if (cmd.generation == 0) {
            ++diag.missingGenerationSurfaceCommands;
            continue;
        }

        minGeneration = std::min(minGeneration, cmd.generation);
        maxGeneration = std::max(maxGeneration, cmd.generation);
        sawGeneration = true;
    }

    if (sawGeneration) {
        diag.minSurfaceGeneration = minGeneration;
        diag.maxSurfaceGeneration = maxGeneration;
    }
}

} // namespace

Scene::Scene()
    : camera_(std::make_unique<Camera>()),
      cameraController_(std::make_unique<CameraController>(camera_.get())),
      debugOverlay_(std::make_unique<DebugOverlay>()),
      inputManager_(std::make_unique<InputManager>()),
      pickingService_(std::make_unique<PickingService>()),
      selectionManager_(std::make_unique<SelectionManager>()),
      timeController_(std::make_unique<TimeController>()),
      skyGradient_(std::make_unique<SkyGradient>()),
      atmospherePass_(std::make_unique<AtmosphereBackgroundPass>()),
      skyBox_(std::make_unique<SkyBox>()) {

    // OpenGlobus Camera defaults to a near plane of 1m and minAltitude=1m.
    // OpenGlobus PlanetCamera reverse-Z defaults: near=150, far=1e12.
    // Camera constructor already sets these; depth func/clear are configured
    // per-platform by RenderDevice.
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
        // Context lost: 标记所有 GPU 资源失效
        // 图层纹理缓存在下次 surfaceCreated + Renderer 重建后自然重建
        // (TileTextureCache 在 Renderer 重建时重新分配)
        // TerrainLayer 只保留 CPU TerrainTile 数据；SurfaceTile GPU mesh
        // 由 BasemapLayer 在新 RenderDevice 上按 terrainGeneration 重建。
        renderer_.reset();
        return false;
    }

    renderer_ = std::make_unique<Renderer>(device);
    if (!renderer_->initialize(globeMesh_)) {
        fprintf(stderr, "[Scene] renderer_->initialize() FAILED\n");
        return false;
    }

    // 初始化调试叠加层
    debugOverlay_->initialize(device);

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
    if (cameraController_) {
        cameraController_->update(deltaSeconds);
    }

    elapsedTime_ += deltaSeconds;
    frameState_.frameId = ++frameId_;
    frameState_.timeSeconds = elapsedTime_;
    frameState_.camera = camera_.get();

    // 更新 FPS（5 帧平滑）
    if (deltaSeconds > 0.0) {
        frameState_.diagnostics.frameTimeMs = deltaSeconds * 1000.0;
        constexpr double kFpsSmoothing = 0.1;
        frameState_.diagnostics.fps =
            frameState_.diagnostics.fps * (1.0 - kFpsSmoothing) +
            (1.0 / deltaSeconds) * kFpsSmoothing;
    }

    // 环境系统：更新太阳方向 + 天空颜色
    Vec3 sunDir = SunDirection::compute(timeController_->julianDate());
    double camAlt = camera_->getHeight();
    skyGradient_->update(sunDir, camAlt);
    frameState_.lightDir = {
        static_cast<float>(sunDir.x()),
        static_cast<float>(sunDir.y()),
        static_cast<float>(sunDir.z())
    };
    auto& hc = skyGradient_->horizonColor();
    frameState_.clearR = hc[0];
    frameState_.clearG = hc[1];
    frameState_.clearB = hc[2];

    // 使用图层栈统一驱动更新（分组共享 TilePlan）
    layerStack_.update(frameState_);

    // 地形更新
    if (terrainLayer_ && terrainEnabled_) {
        terrainLayer_->update(frameState_);
    }
}

void Scene::render() {
    if (!renderer_ || !isReady()) return;

    RenderCommandList commands;
    fprintf(stderr, "[Scene::render] entered | sky=%d atmo=%d layers=%zu\n",
        skyBox_ ? skyBox_->isReady() : -1,
        atmospherePass_ ? atmospherePass_->isReady() : -1,
        layerStack_.layers().size());

    // 0. SkyBox（最远）
    if (skyBox_ && skyBox_->isReady()) {
        const auto& cam = camera();
        Mat4 vm = cam.viewMatrix();  // must store, .raw() refs internals
        const double* vmPtr = glm::value_ptr(vm.raw());
        float viewMatrix[16];
        for (int i = 0; i < 16; ++i) viewMatrix[i] = static_cast<float>(vmPtr[i]);
        float vpW = static_cast<float>(frameState_.viewportWidthPixels);
        float vpH = static_cast<float>(frameState_.viewportHeightPixels);
        Mat4 pm = cam.projectionMatrix(
            static_cast<double>(vpW), static_cast<double>(vpH));
        const double* pmPtr = glm::value_ptr(pm.raw());
        float projMatrix[16];
        for (int i = 0; i < 16; ++i) projMatrix[i] = static_cast<float>(pmPtr[i]);
        // Night factor: 0 when sun above horizon, ramps to 1 below.
        // In high orbit, keep the starfield visible behind the atmosphere
        // instead of a flat daytime clear color.
        float nightFactor = static_cast<float>(
            skyGradient_->sunElevation() < -0.05
                ? std::clamp(std::exp(skyGradient_->sunElevation() * 8.0), 0.0, 1.0)
                : 0.0);
        double spaceFactor = std::clamp((cam.getHeight() - 120000.0) / 780000.0, 0.0, 1.0);
        spaceFactor = spaceFactor * spaceFactor * (3.0 - 2.0 * spaceFactor);
        nightFactor = std::max(nightFactor, static_cast<float>(spaceFactor));
        commands.push_back(skyBox_->buildCommand(
            viewMatrix, projMatrix, cam.isOrthographic(), nightFactor));
    }

    // 0.5 AtmosphereBackgroundPass（SkyBox 之上，地球之下）
    if (atmospherePass_ && atmospherePass_->isReady()) {
        const auto& cam = camera();
        float vpW = static_cast<float>(frameState_.viewportWidthPixels);
        float vpH = static_cast<float>(frameState_.viewportHeightPixels);

        Vec3 sunDir(
            frameState_.lightDir.x,
            frameState_.lightDir.y,
            frameState_.lightDir.z);

        commands.push_back(atmospherePass_->buildCommand(
            cam.position(),
            static_cast<float>(cam.verticalFovRadians()),
            static_cast<int>(vpW),
            static_cast<int>(vpH),
            cam.right(),
            cam.up(),
            cam.direction(),
            sunDir,
            skyGradient_->parameters()));
    }

    // 1. 标准底图 SurfaceTile 主链路。地形启用时，TerrainLayer 只作为
    // SurfaceTile mesh 的高度数据源，不再发出独立地形 surface pass。
    const TerrainLayer* terrainSource =
        (terrainLayer_ && terrainEnabled_ && terrainLayer_->visible())
            ? terrainLayer_.get()
            : nullptr;
    layerStack_.buildRenderCommands(*renderer_, terrainSource, commands);

    const bool hasSurfaceTile = std::any_of(commands.begin(), commands.end(),
        [](const RenderCommand& cmd) {
            return cmd.kind == RenderCommandKind::SurfaceTile;
        });
    if (!hasSurfaceTile && !(terrainLayer_ && terrainEnabled_ && terrainLayer_->visible())) {
        commands.insert(commands.begin(), renderer_->makeGlobeCommand(frameState_));
    }

    // 2. 矢量图层
    for (auto& vLayer : vectorLayers_) {
        if (vLayer->visible()) {
            vLayer->buildRenderCommands(frameState_, *renderer_, commands);
        }
    }

    // 3. 调试叠加层
    if (debugOverlay_ && debugOverlay_->enabled()) {
        for (const auto& layer : layerStack_.layers()) {
            if (!layer->visible()) continue;
            debugOverlay_->buildCommands(
                layer->layerPlan(),
                layer->tileScheme(),
                commands);
        }
    }

    // 4. 后处理：为 tile/debug commands 设置 MVP
    if (frameState_.camera) {
        const Camera& cam = *frameState_.camera;
        float vpW = static_cast<float>(frameState_.viewportWidthPixels);
        float vpH = static_cast<float>(frameState_.viewportHeightPixels);

        glm::mat4 view(cam.viewMatrix().raw());
        glm::mat4 proj(cam.projectionMatrix(
            static_cast<double>(vpW), static_cast<double>(vpH)).raw());
        glm::mat4 viewProj = proj * view;
        glm::mat4 relativeView = glm::mat4(glm::mat3(view));
        glm::mat4 relativeViewProj = proj * relativeView;

        for (auto& cmd : commands) {
            if (cmd.owner == "globe") continue;
            auto& mvpU = cmd.uniforms["u_modelViewProjection"];
            if (mvpU.empty()) {
                mvpU.resize(16);
                const glm::mat4& matrix = (cmd.owner == "surface_tile")
                    ? relativeViewProj
                    : viewProj;
                std::memcpy(mvpU.data(), glm::value_ptr(matrix), 16 * sizeof(float));
            }
            if (cmd.owner == "surface_tile") {
                cmd.uniforms["u_lightDir"] = {
                    frameState_.lightDir.x,
                    frameState_.lightDir.y,
                    frameState_.lightDir.z
                };
                auto& originU = cmd.uniforms["u_cameraRelativeOrigin"];
                if (originU.empty()) {
                    originU = {
                        static_cast<float>(cam.position().x()),
                        static_cast<float>(cam.position().y()),
                        static_cast<float>(cam.position().z())
                    };
                }
            }
        }
    }

    sortMvpRenderCommands(commands);
    updateSurfaceCommandDiagnostics(
        commands, frameState_.frameId, frameState_.diagnostics);
    if (auto error = validateMvpRenderCommands(commands, frameState_.frameId)) {
        throw std::runtime_error(
            "MVP render command validation failed for '" + error->owner +
            "': " + error->message);
    }

    // 5. 填充诊断数据
    auto& diag = frameState_.diagnostics;
    diag.drawCalls = static_cast<int>(commands.size());
    diag.visibleTiles = 0;
    diag.cachedTextures = 0;
    diag.queuedRequests = 0;
    diag.loadingRequests = 0;
    diag.gpuTextureCount = 0;
    diag.renderSurfaceTiles = 0;
    diag.surfaceMeshCount = 0;
    diag.imageryAttachments = 0;
    diag.imageryExactAttachments = 0;
    diag.imageryParentFallbackAttachments = 0;
    diag.imageryMissingTiles = 0;
    diag.imageryUnsupportedTiles = 0;
    diag.imageryTransitionTiles = 0;
    diag.imageryKickedTiles = 0;
    diag.imageryAncestorRetainedTiles = 0;
    diag.imageryMinTargetZoom = 0;
    diag.imageryMaxTargetZoom = 0;
    diag.imageryMinTextureZoom = 0;
    diag.imageryMaxTextureZoom = 0;
    diag.lodSizePixels = 0.0;
    diag.minVisibleZoom = 0;
    diag.maxVisibleZoom = 0;
    diag.quadtreeEqualZoomLayers = 0;
    diag.quadtreeFadingNodes = 0;
    diag.quadtreeNeighborLinks = 0;
    diag.quadtreeNeighborBalancedTiles = 0;
    diag.quadtreeRenderingNodes = 0;
    diag.quadtreeWalkthroughNodes = 0;
    diag.quadtreeNotRenderingNodes = 0;
    diag.quadtreeSelectionRenderedNodes = 0;
    diag.quadtreeSelectionRefinedNodes = 0;
    diag.quadtreeSelectionKickedNodes = 0;
    diag.quadtreeSelectionAncestorMeetsSseNodes = 0;
    diag.quadtreeCameraInsideNodes = 0;
    diag.quadtreeInFrustumNodes = 0;
    diag.quadtreeHorizonTangentPreservedNodes = 0;
    diag.quadtreeEqualZoomSecondPassNodes = 0;
    diag.mercatorTileCount = 0;
    diag.northPolarTileCount = 0;
    diag.southPolarTileCount = 0;
    diag.surfaceMeshBytes = 0;
    diag.terrainCachedTiles = 0;
    diag.terrainGeneration = 0;
    diag.terrainSurfaceMeshes = 0;
    diag.terrainParentFallbackMeshes = 0;
    diag.terrainReadySurfaceMeshes = 0;
    diag.terrainTransitionSurfaceMeshes = 0;
    diag.ellipsoidSurfaceMeshes = 0;
    for (const auto& layer : layerStack_.layers()) {
        diag.visibleTiles += layer->visibleTileCount();
        diag.cachedTextures += layer->cachedTileCount();
        diag.queuedRequests += layer->requestTileCount();
        diag.renderSurfaceTiles += layer->renderTileCount();
        diag.surfaceMeshCount += layer->surfaceMeshCount();
        diag.imageryExactAttachments += layer->exactAttachmentCount();
        diag.imageryParentFallbackAttachments += layer->parentFallbackAttachmentCount();
        diag.imageryMissingTiles += layer->missingImageryTileCount();
        diag.imageryUnsupportedTiles += layer->unsupportedImageryTileCount();
        diag.imageryTransitionTiles += layer->transitionTileCount();
        diag.imageryKickedTiles += layer->kickedTileCount();
        diag.imageryAncestorRetainedTiles += layer->ancestorRetainedTileCount();
        if (layer->renderTileCount() > 0) {
            diag.imageryMinTargetZoom = diag.imageryMinTargetZoom == 0
                ? layer->minRenderTargetZoom()
                : std::min(diag.imageryMinTargetZoom, layer->minRenderTargetZoom());
            diag.imageryMaxTargetZoom =
                std::max(diag.imageryMaxTargetZoom, layer->maxRenderTargetZoom());
            diag.imageryMinTextureZoom = diag.imageryMinTextureZoom == 0
                ? layer->minRenderTextureZoom()
                : std::min(diag.imageryMinTextureZoom, layer->minRenderTextureZoom());
            diag.imageryMaxTextureZoom =
                std::max(diag.imageryMaxTextureZoom, layer->maxRenderTextureZoom());
        }
        diag.lodSizePixels = std::max(diag.lodSizePixels, layer->lodSizePixels());
        if (layer->visibleTileCount() > 0) {
            diag.minVisibleZoom = diag.minVisibleZoom == 0
                ? layer->minVisibleZoom()
                : std::min(diag.minVisibleZoom, layer->minVisibleZoom());
            diag.maxVisibleZoom = std::max(diag.maxVisibleZoom, layer->maxVisibleZoom());
        }
        diag.quadtreeEqualZoomLayers += layer->quadtreeEqualZoomApplied();
        diag.quadtreeFadingNodes += layer->quadtreeFadingNodeCount();
        diag.quadtreeNeighborLinks += layer->quadtreeNeighborLinkCount();
        diag.quadtreeNeighborBalancedTiles +=
            layer->quadtreeNeighborBalancedTileCount();
        diag.quadtreeRenderingNodes += layer->quadtreeRenderingNodeCount();
        diag.quadtreeWalkthroughNodes += layer->quadtreeWalkthroughNodeCount();
        diag.quadtreeNotRenderingNodes += layer->quadtreeNotRenderingNodeCount();
        diag.quadtreeSelectionRenderedNodes +=
            layer->layerPlan().quadtreeSelectionRenderedCount;
        diag.quadtreeSelectionRefinedNodes +=
            layer->layerPlan().quadtreeSelectionRefinedCount;
        diag.quadtreeSelectionKickedNodes +=
            layer->layerPlan().quadtreeSelectionKickedCount;
        diag.quadtreeSelectionAncestorMeetsSseNodes +=
            layer->layerPlan().quadtreeSelectionAncestorMeetsSseCount;
        diag.quadtreeCameraInsideNodes += layer->quadtreeCameraInsideNodeCount();
        diag.quadtreeInFrustumNodes += layer->quadtreeInFrustumNodeCount();
        diag.quadtreeHorizonTangentPreservedNodes +=
            layer->quadtreeHorizonTangentPreservedCount();
        diag.quadtreeEqualZoomSecondPassNodes +=
            layer->quadtreeEqualZoomSecondPassNodeCount();
        diag.mercatorTileCount += layer->mercatorTileCount();
        diag.northPolarTileCount += layer->northPolarTileCount();
        diag.southPolarTileCount += layer->southPolarTileCount();
        diag.surfaceMeshBytes += static_cast<int>(layer->surfaceMeshBytes());
        diag.terrainSurfaceMeshes += layer->terrainSurfaceMeshCount();
        diag.terrainParentFallbackMeshes += layer->terrainParentFallbackMeshCount();
        diag.terrainReadySurfaceMeshes += layer->terrainReadySurfaceMeshCount();
        diag.terrainTransitionSurfaceMeshes += layer->terrainTransitionSurfaceMeshCount();
        diag.ellipsoidSurfaceMeshes += layer->ellipsoidSurfaceMeshCount();
    }
    diag.loadingRequests = diag.queuedRequests;
    diag.gpuTextureCount = diag.cachedTextures;
    diag.imageryAttachments =
        diag.imageryExactAttachments + diag.imageryParentFallbackAttachments;
    if (terrainLayer_ && terrainEnabled_) {
        diag.terrainCachedTiles = terrainLayer_->cachedTileCount();
        diag.terrainGeneration = terrainLayer_->terrainGeneration();
    }

    // 6. 提交
    fprintf(stderr, "[Scene::render] submitting %zu commands\n", commands.size());
    renderer_->submit(commands);
}

// ---- 底图图层管理 ----

void Scene::addLayer(std::unique_ptr<BasemapLayer> layer) {
    layerStack_.addLayer(std::move(layer));
}

std::unique_ptr<BasemapLayer> Scene::removeLayer(const std::string& layerId) {
    return layerStack_.removeLayer(layerId);
}

void Scene::moveLayer(const std::string& layerId, size_t index) {
    layerStack_.moveLayer(layerId, index);
}

size_t Scene::layerCount() const {
    return layerStack_.layerCount();
}

std::vector<BasemapLayerStack::ControlPointResult>
Scene::verifyControlPoint(double lngRad, double latRad, int zoom) const {
    return layerStack_.verifyControlPoint(lngRad, latRad, zoom);
}

// ---- 地形 ----

void Scene::setTerrainLayer(std::unique_ptr<TerrainLayer> layer) {
    terrainLayer_ = std::move(layer);
    configureCameraSurfacePicker();
}

void Scene::setTerrainEnabled(bool enabled) {
    terrainEnabled_ = enabled;
    configureCameraSurfacePicker();
}

bool Scene::hasTerrain() const {
    return terrainLayer_ != nullptr;
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

    // 地形采样器
    std::function<float(double,double)> terrainSampler;
    if (terrainLayer_ && terrainEnabled_) {
        terrainSampler = [this](double lng, double lat) {
            return terrainLayer_->sampleHeight(lng, lat);
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

            std::function<float(double,double)> terrainSampler;
            if (terrainLayer_ && terrainEnabled_) {
                terrainSampler = [this](double lng, double lat) {
                    return terrainLayer_->sampleHeight(lng, lat);
                };
            }

            PickResult result = pickingService_->pickTerrain(
                screenX, screenY,
                *camera_,
                static_cast<double>(frameState_.viewportWidthPixels),
                static_cast<double>(frameState_.viewportHeightPixels),
                terrainSampler);
            if (!result.isValid()) {
                return false;
            }

            outPoint = result.worldPosition;
            return true;
        });

    // Terrain collision: inject terrain height query for camera floor checks.
    cameraController_->setTerrainHeightFunc(
        [this](const Vec3& ecefPosition) -> double {
            if (!terrainLayer_ || !terrainEnabled_) return 0.0;
            const auto& ellipsoid = Ellipsoid::WGS84();
            const Cartographic c = ellipsoid.cartesianToCartographic(ecefPosition);
            return static_cast<double>(
                terrainLayer_->sampleHeight(
                    static_cast<float>(c.longitude()),
                    static_cast<float>(c.latitude())));
        });
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

void Scene::onInputEvent(const InputEvent& event) {
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

// ---- 调试叠加层 ----

void Scene::setDebugOverlayEnabled(bool enabled) {
    debugOverlay_->setEnabled(enabled);
}

bool Scene::debugOverlayEnabled() const {
    return debugOverlay_->enabled();
}

} // namespace earth_engine
