#pragma once

#include "FrameState.h"
#include "SceneInputCoordinator.h"
#include "ScenePickingCoordinator.h"

#include "../interaction/InputManager.h"
#include "../interaction/PickingService.h"
#include "../interaction/SelectionManager.h"

#include <functional>
#include <memory>
#include <string>

namespace earth_engine {

class Camera;
class CameraSystem;
class Tileset;
class VectorLayer;
class FeatureRenderLayer;

struct SceneInteractionContext {
    Camera* camera = nullptr;
    CameraSystem* cameraSystem = nullptr;
    double viewportWidthPixels = 0.0;
    double viewportHeightPixels = 0.0;
    const Tileset* terrainTileset = nullptr;
    const std::vector<std::unique_ptr<VectorLayer>>* vectorLayers = nullptr;
    const FrameState* frameState = nullptr;
    const std::vector<std::unique_ptr<FeatureRenderLayer>>*
        featureRenderLayers = nullptr;
    double elapsedTimeSeconds = 0.0;
};

class SceneInteractionCoordinator {
public:
    using FeatureStateChangeCallback = std::function<void(
        const std::string&,
        const std::string&,
        FeatureState)>;

    SceneInteractionCoordinator();

    void setFeatureStateChangeCallback(FeatureStateChangeCallback callback);
    void configureCameraSurfacePicker(
        CameraSystem& cameraSystem,
        std::function<SceneInteractionContext()> contextProvider);

    PickResult pick(const SceneInteractionContext& context,
                    float screenX,
                    float screenY) const;
    bool pickInteractionFocus(const SceneInteractionContext& context,
                              float screenX,
                              float screenY,
                              Vec3& outPoint) const;

    void onInputEvent(const SceneInteractionContext& context,
                      const InputEvent& event);
    void onHover(const PickResult& result);
    void onSelect(const PickResult& result);
    void clearSelection();

    bool hasInteractionFocus() const { return focusState_.hasFocus; }
    const Vec3& interactionFocusDirection() const {
        return focusState_.direction;
    }
    double interactionFocusTimeSeconds() const {
        return focusState_.timeSeconds;
    }

private:
    ScenePickingContext pickingContext(
        const SceneInteractionContext& context) const;
    SceneInputCoordinatorContext inputContext(
        const SceneInteractionContext& context) const;
    void setupInputCallback();
    void updateInteractionFocus(const SceneInteractionContext& context,
                                const InputEvent& event);

    std::unique_ptr<InputManager> inputManager_;
    std::unique_ptr<PickingService> pickingService_;
    std::unique_ptr<SelectionManager> selectionManager_;
    SceneInteractionFocusState focusState_;
    const SceneInteractionContext* activeInputContext_ = nullptr;
};

} // namespace earth_engine
