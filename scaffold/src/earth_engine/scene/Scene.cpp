#include "Scene.h"
#include "Camera.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstring>
#include <algorithm>

namespace earth_engine {

Scene::Scene()
    : camera_(std::make_unique<Camera>()),
      cameraController_(std::make_unique<CameraController>(camera_.get())),
      debugOverlay_(std::make_unique<DebugOverlay>()),
      inputManager_(std::make_unique<InputManager>()),
      pickingService_(std::make_unique<PickingService>()),
      selectionManager_(std::make_unique<SelectionManager>()),
      timeController_(std::make_unique<TimeController>()),
      skyGradient_(std::make_unique<SkyGradient>()) {

    // 近平面 10000m：地球尺度渲染需要足够的深度精度
    // 1m 近平面会将 99%+ 的深度范围浪费在前几公里
    camera_->setPerspective(glm::radians(60.0), 10000.0, 50000000.0);
    globeMesh_ = Globe::createMesh(96, 48);
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
        // 地形网格标记为脏，下次渲染时重建
        if (terrainLayer_) {
            // terrainLayer_ 的 tileCache_ 持有 TerrainTile CPU 数据，可保留
            // 但下次渲染需重新上传 mesh（通过 Renderer 的 updateGlobeVertices）
        }
        renderer_.reset();
        return false;
    }

    renderer_ = std::make_unique<Renderer>(device);
    if (!renderer_->initialize(globeMesh_)) return false;

    // 初始化调试叠加层
    debugOverlay_->initialize(device);

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
    skyGradient_->update(sunDir);
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

    // 1. Globe 背景（或地形如果启用）
    static bool sWasTerrainLastFrame = false;
    if (terrainLayer_ && terrainEnabled_ && terrainLayer_->visible()) {
        terrainLayer_->buildRenderCommands(globeMesh_, frameState_,
                                            *renderer_, commands);
        sWasTerrainLastFrame = true;
    } else {
        // 地形→平坦切换时恢复原始椭球顶点+索引 buffer（避免每帧重建）
        if (sWasTerrainLastFrame) {
            renderer_->updateGlobeMesh(globeMesh_);
            sWasTerrainLastFrame = false;
        }
        commands.push_back(renderer_->makeGlobeCommand(frameState_));
    }

    // 2. 底图图层（通过栈按顺序生成渲染命令）
    layerStack_.buildRenderCommands(*renderer_, commands);

    // 3. 矢量图层
    for (auto& vLayer : vectorLayers_) {
        if (vLayer->visible()) {
            vLayer->buildRenderCommands(frameState_, *renderer_, commands);
        }
    }

    // 4. 调试叠加层
    if (debugOverlay_ && debugOverlay_->enabled()) {
        for (const auto& [layerId, tiles] : layerStack_.allVisibleTiles()) {
            auto* layer = layerStack_.findLayer(layerId);
            if (layer) {
                debugOverlay_->buildCommands(
                    tiles,
                    layer->tileScheme(),
                    commands);
            }
        }
    }

    // 5. 后处理：为 tile/debug commands 设置 MVP
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
                const glm::mat4& matrix = (cmd.owner == "tile")
                    ? relativeViewProj
                    : viewProj;
                std::memcpy(mvpU.data(), glm::value_ptr(matrix), 16 * sizeof(float));
            }
            if (cmd.owner == "tile") {
                cmd.uniforms["u_cameraRelativeOrigin"] = {
                    static_cast<float>(cam.position().x()),
                    static_cast<float>(cam.position().y()),
                    static_cast<float>(cam.position().z())
                };
            }
        }
    }

    // 6. 填充诊断数据
    auto& diag = frameState_.diagnostics;
    diag.drawCalls = static_cast<int>(commands.size());
    diag.visibleTiles = 0;
    diag.cachedTextures = 0;
    for (const auto& layer : layerStack_.layers()) {
        diag.visibleTiles += layer->visibleTileCount();
        diag.cachedTextures += layer->cachedTileCount();
    }

    // 7. 提交
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
}

void Scene::setTerrainEnabled(bool enabled) {
    terrainEnabled_ = enabled;
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
                    cameraController_->onPinch(event.pinchScale);
                    break;
                case InputManager::Gesture::PinchMove:
                    cameraController_->onPinch(event.pinchScale);
                    break;
                case InputManager::Gesture::PinchEnd:
                    // pinch 结束重置
                    cameraController_->onPinch(1.0f);
                    break;
                case InputManager::Gesture::Click:
                case InputManager::Gesture::DoubleClick: {
                    PickResult result = pick(event.screenX, event.screenY);
                    if (gesture == InputManager::Gesture::DoubleClick) {
                        // 双击：若有命中则选中，否则放大
                        if (result.isValid()) {
                            selectionManager_->onSelect(result);
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
