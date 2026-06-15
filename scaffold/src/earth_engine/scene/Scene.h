#pragma once

#include "FrameState.h"
#include "../camera/CameraController.h"
#include "../globe/Globe.h"
#include "../renderer/Renderer.h"
#include "../layers/VectorLayer.h"
#include "../tiling/Tileset.h"
#include "../interaction/InputEvent.h"
#include "../interaction/InputManager.h"
#include "../interaction/PickingService.h"
#include "../interaction/SelectionManager.h"
#include "../environment/TimeController.h"
#include "../environment/SunDirection.h"
#include "../environment/SkyGradient.h"
#include "../environment/AtmosphereBackgroundPass.h"
#include "../environment/SkyBox.h"
#include <memory>
#include <vector>
#include <string>

namespace earth_engine {

class Camera;
class RenderDevice;

/// 3D 场景管理器。
class Scene {
public:
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
    void setOcclusionCallback(Tileset::OcclusionCallback callback);
    void clearOcclusionCallback();

    const FrameState& frameState() const { return frameState_; }

    /// 运行时诊断（FPS、draw calls、visible tiles 等）
    const Diagnostics& diagnostics() const { return frameState_.diagnostics; }
    Diagnostics& mutableDiagnostics() { return frameState_.diagnostics; }

    // ---- 矢量图层管理 ----
    void addVectorLayer(std::unique_ptr<VectorLayer> layer);
    std::unique_ptr<VectorLayer> removeVectorLayer(const std::string& layerId);
    size_t vectorLayerCount() const { return vectorLayers_.size(); }

    // ---- 统一 Tileset（cesium-native 对齐） ----
    void setTileset(std::unique_ptr<Tileset> tileset);
    void addTileset(std::unique_ptr<Tileset> tileset);
    Tileset* tileset() const { return tileset_.get(); }
    size_t additionalTilesetCount() const { return additionalTilesets_.size(); }
    bool hasTerrain() const { return tileset_ != nullptr; }

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
    const SkyGradient& skyGradient() const { return *skyGradient_; }

private:
    void configureCameraSurfacePicker();
    void setupSelectionCallbacks();
    void setupInputCallback();
    bool pickInteractionFocus(float screenX, float screenY, Vec3& outPoint) const;
    void updateInteractionFocus(const InputEvent& event);
    void populateSelectorViews();

    std::unique_ptr<Camera> camera_;
    std::unique_ptr<CameraController> cameraController_;
    std::unique_ptr<Renderer> renderer_;
    GlobeMesh globeMesh_;
    FrameState frameState_;
    RenderCommandList renderCommands_;
    RenderDevice* renderDevice_ = nullptr;
    uint64_t frameId_ = 0;
    double elapsedTime_ = 0.0;
    bool hasSelectorViewOverride_ = false;
    std::vector<FrameState::SelectorView> selectorViewOverride_;

    // 矢量图层
    std::vector<std::unique_ptr<VectorLayer>> vectorLayers_;

    // 统一 Tileset（cesium-native 对齐）
    std::unique_ptr<Tileset> tileset_;
    std::vector<std::unique_ptr<Tileset>> additionalTilesets_;
    Tileset::OcclusionCallback occlusionCallback_;

    // 交互
    std::unique_ptr<InputManager> inputManager_;
    std::unique_ptr<PickingService> pickingService_;
    std::unique_ptr<SelectionManager> selectionManager_;
    bool hasInteractionFocus_ = false;
    Vec3 interactionFocusDirection_ = Vec3::zero();
    double interactionFocusTimeSeconds_ = -1.0;

    // 环境系统
    std::unique_ptr<TimeController> timeController_;
    std::unique_ptr<SkyGradient> skyGradient_;
    std::unique_ptr<AtmosphereBackgroundPass> atmospherePass_;
    std::unique_ptr<SkyBox> skyBox_;
};

} // namespace earth_engine
