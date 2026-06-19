#include "SceneInputCoordinator.h"

#include "../camera/CameraController.h"
#include "../interaction/SelectionManager.h"

namespace earth_engine {
namespace {

void selectFromClick(const SceneInputCoordinatorContext& context,
                     const InputEvent& event,
                     const PickResult& result) {
    SelectionManager* selectionManager = context.selectionManager;
    if (!selectionManager) {
        return;
    }
    if (!result.isValid()) {
        selectionManager->clearSelection();
        return;
    }
    if (event.modifiers.shift) {
        selectionManager->onSelectAdd(result);
    } else if (event.modifiers.ctrl || event.modifiers.meta) {
        selectionManager->onSelectToggle(result);
    } else {
        selectionManager->onSelect(result);
    }
}

bool shouldUpdateInteractionFocus(InputEvent::Type type) {
    switch (type) {
        case InputEvent::Type::PointerDown:
        case InputEvent::Type::PointerMove:
        case InputEvent::Type::PointerUp:
        case InputEvent::Type::PinchStart:
        case InputEvent::Type::PinchMove:
        case InputEvent::Type::PinchEnd:
            return true;
        default:
            return false;
    }
}

} // namespace

void SceneInputCoordinator::handleGesture(
    const SceneInputCoordinatorContext& context,
    InputManager::Gesture gesture,
    const InputEvent& event) {
    CameraController* cameraController = context.cameraController;
    if (!cameraController) {
        return;
    }

    switch (gesture) {
        case InputManager::Gesture::DragStart:
            cameraController->onDragStart(
                event.screenX,
                event.screenY,
                event.timestamp);
            break;
        case InputManager::Gesture::DragMove:
            cameraController->onDragMove(
                event.screenX,
                event.screenY,
                event.timestamp);
            break;
        case InputManager::Gesture::DragEnd:
            cameraController->onDragEnd();
            break;
        case InputManager::Gesture::PinchStart:
        case InputManager::Gesture::PinchMove:
            cameraController->onPinchGesture(
                event.pinchScale,
                event.screenX,
                event.screenY,
                event.rotationRadians,
                event.centerDeltaX,
                event.centerDeltaY,
                event.timestamp);
            break;
        case InputManager::Gesture::PinchEnd:
            cameraController->onPinchEnd();
            break;
        case InputManager::Gesture::Click:
        case InputManager::Gesture::DoubleClick: {
            const PickResult result = context.pick
                ? context.pick(event.screenX, event.screenY)
                : PickResult{};
            if (gesture == InputManager::Gesture::DoubleClick) {
                if (result.isValid()) {
                    cameraController->viewDistance(
                        result.worldPosition,
                        result.distance * 0.57);
                } else {
                    cameraController->setDistance(
                        cameraController->distance() * 0.7f);
                }
            } else {
                selectFromClick(context, event, result);
            }
            break;
        }
    }
}

void SceneInputCoordinator::updateInteractionFocus(
    const SceneInputCoordinatorContext& context,
    const InputEvent& event,
    SceneInteractionFocusState& focusState) {
    if (!shouldUpdateInteractionFocus(event.type) ||
        !context.pickInteractionFocus) {
        return;
    }

    Vec3 focusPoint;
    if (!context.pickInteractionFocus(
            event.screenX,
            event.screenY,
            focusPoint)) {
        return;
    }
    focusState.direction = focusPoint.normalized();
    focusState.timeSeconds = context.elapsedTimeSeconds;
    focusState.hasFocus = true;
}

} // namespace earth_engine
