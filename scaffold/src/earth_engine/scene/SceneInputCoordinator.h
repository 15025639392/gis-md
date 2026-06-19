#pragma once

#include "../core/math/Vec3.h"
#include "../interaction/InputEvent.h"
#include "../interaction/InputManager.h"
#include "../interaction/PickingService.h"

#include <functional>

namespace earth_engine {

class CameraController;
class SelectionManager;

struct SceneInteractionFocusState {
    bool hasFocus = false;
    Vec3 direction = Vec3::zero();
    double timeSeconds = -1.0;
};

struct SceneInputCoordinatorContext {
    CameraController* cameraController = nullptr;
    SelectionManager* selectionManager = nullptr;
    std::function<PickResult(float, float)> pick;
    std::function<bool(float, float, Vec3&)> pickInteractionFocus;
    double elapsedTimeSeconds = 0.0;
};

class SceneInputCoordinator {
public:
    static void handleGesture(const SceneInputCoordinatorContext& context,
                              InputManager::Gesture gesture,
                              const InputEvent& event);
    static void updateInteractionFocus(
        const SceneInputCoordinatorContext& context,
        const InputEvent& event,
        SceneInteractionFocusState& focusState);
};

} // namespace earth_engine
