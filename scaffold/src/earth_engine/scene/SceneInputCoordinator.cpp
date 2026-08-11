#include "SceneInputCoordinator.h"

#include "../camera/CameraSystem.h"
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

CameraSystem::PinchMode toCameraPinchMode(InputEvent::PinchMode mode) {
    switch (mode) {
        case InputEvent::PinchMode::Undecided:
            return CameraSystem::PinchMode::Undecided;
        case InputEvent::PinchMode::Pitch:
            return CameraSystem::PinchMode::Pitch;
        case InputEvent::PinchMode::Manipulate:
            break;
    }
    return CameraSystem::PinchMode::Manipulate;
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
    CameraSystem* cameraSystem = context.cameraSystem;
    if (!cameraSystem) {
        return;
    }

    switch (gesture) {
        case InputManager::Gesture::DragStart:
            cameraSystem->onDragStart(
                event.screenX,
                event.screenY,
                event.timestamp);
            break;
        case InputManager::Gesture::DragMove:
            cameraSystem->onDragMove(
                event.screenX,
                event.screenY,
                event.timestamp);
            break;
        case InputManager::Gesture::DragEnd:
            cameraSystem->onDragEnd();
            break;
        case InputManager::Gesture::PinchStart:
        case InputManager::Gesture::PinchMove:
            if (event.hasPointerPair) {
                // 新契约：InputManager 已算好绝对派生量与 latch 模式。
                CameraSystem::PinchInput input;
                input.scaleFromStart = event.pinchScaleFromStart;
                input.twistFromStartRadians = event.twistFromStartRadians;
                input.centroidX = (event.pointer0X + event.pointer1X) * 0.5f;
                input.centroidY = (event.pointer0Y + event.pointer1Y) * 0.5f;
                input.mode = toCameraPinchMode(event.pinchMode);
                input.timestamp = event.timestamp;
                cameraSystem->onPinchGesture(input);
            } else {
                // 旧契约（无 pointer pair 的平台/合成路径）：走适配器。
                cameraSystem->onPinchGesture(
                    event.pinchScale,
                    event.screenX,
                    event.screenY,
                    event.rotationRadians,
                    event.centerDeltaX,
                    event.centerDeltaY,
                    event.timestamp);
            }
            break;
        case InputManager::Gesture::PinchEnd:
            cameraSystem->onPinchEnd();
            break;
        case InputManager::Gesture::Click:
        case InputManager::Gesture::DoubleClick: {
            const PickResult result = context.pick
                ? context.pick(event.screenX, event.screenY)
                : PickResult{};
            if (gesture == InputManager::Gesture::DoubleClick) {
                // 未命中椭球 = 双击在天空/太空上，没有地理语义 ⇒ 不响应。
                // 旧行为是 setDistance(distance*0.7)，它翻开 orbit 模式，下一帧
                // 位姿被重建成"看向地心"，用户的 tilt/heading 当场丢光；低空时
                // 0.7×地心距还会跌破下限被钳到地表 50m 正俯视。orbit 表示已删。
                if (result.isValid()) {
                    cameraSystem->viewDistance(
                        result.worldPosition,
                        result.distance * 0.57);
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
