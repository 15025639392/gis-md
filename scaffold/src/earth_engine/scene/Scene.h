#pragma once

#include "EngineTimingScope.h"
#include "FrameState.h"
#include "SceneFrameRuntime.h"
#include "../tiling/TileOcclusionCallback.h"
#include <memory>
#include <vector>
#include <string>

namespace earth_engine {

class Camera;
class CameraController;
struct Diagnostics;
struct GlobeMesh;
struct InputEvent;
struct PickResult;
struct PresentationTrace;
class RenderDevice;
class Renderer;
class SceneEnvironmentCoordinator;
class SceneLayerCoordinator;
struct SceneInteractionContext;
class SceneInteractionCoordinator;
class SceneRenderPipeline;
class SceneTelemetryCoordinator;
class SceneTilesetCoordinator;
class SkyGradient;
class Tileset;
class VectorLayer;
class Vec3;

/// 3D 场景管理器。
class Scene {
public:
    using EngineTimingScope = earth_engine::EngineTimingScope;

    Scene();
    ~Scene();

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    bool setRenderDevice(RenderDevice* device);
    bool isReady() const { return renderer_ != nullptr; }

    Camera& camera() { return *camera_; }
    CameraController& cameraController() { return *cameraController_; }

    void setViewport(int widthPixels, int heightPixels, float dpr = 1.0f);
    void update(double deltaSeconds);
    void render();
    void setSelectorViewOverride(
        std::vector<FrameState::SelectorView> selectorViews);
    void clearSelectorViewOverride();
    void setOcclusionCallback(TileOcclusionCallback callback);
    void clearOcclusionCallback();

    const FrameState& frameState() const { return frameRuntime_.frameState(); }

    /// 运行时诊断（FPS、draw calls、visible tiles 等）
    const Diagnostics& diagnostics() const;
    void recordEngineTiming(EngineTimingScope scope, double elapsedMs);
    void finishEngineFrame(double elapsedMs);
    const PresentationTrace& presentationTrace() const;

    // ---- 矢量图层管理 ----
    void addVectorLayer(std::unique_ptr<VectorLayer> layer);
    std::unique_ptr<VectorLayer> removeVectorLayer(const std::string& layerId);
    size_t vectorLayerCount() const;

    // ---- 统一 Tileset（cesium-native 对齐） ----
    void setTileset(std::unique_ptr<Tileset> tileset);
    void addTileset(std::unique_ptr<Tileset> tileset);
    Tileset* tileset() const;
    size_t additionalTilesetCount() const;
    bool hasTerrain() const;

    // ---- 输入事件（归一化） ----
    void onInputEvent(const InputEvent& event);

    // ---- 拾取与选择 ----
    PickResult pick(float screenX, float screenY) const;
    void onHover(const PickResult& result);
    void onSelect(const PickResult& result);
    void clearSelection();

    // ---- 环境系统 ----

    void setTime(double julianDate);
    double time() const;
    void advanceTime(double seconds);
    Vec3 sunDirection() const;
    const SkyGradient& skyGradient() const;

private:
    void configureCameraSurfacePicker();
    SceneInteractionContext interactionContext() const;
    void updatePresentationTrace();

    std::unique_ptr<Camera> camera_;
    std::unique_ptr<CameraController> cameraController_;
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<SceneRenderPipeline> renderPipeline_;
    std::unique_ptr<GlobeMesh> globeMesh_;
    SceneFrameRuntime frameRuntime_;
    RenderDevice* renderDevice_ = nullptr;

    // 矢量图层
    std::unique_ptr<SceneLayerCoordinator> layers_;

    // 统一 Tileset（cesium-native 对齐）
    std::unique_ptr<SceneTilesetCoordinator> tilesets_;

    // 交互
    std::unique_ptr<SceneInteractionCoordinator> interaction_;

    // 环境系统
    std::unique_ptr<SceneEnvironmentCoordinator> environment_;

    // 诊断与 presentation trace
    std::unique_ptr<SceneTelemetryCoordinator> telemetry_;
};

} // namespace earth_engine
