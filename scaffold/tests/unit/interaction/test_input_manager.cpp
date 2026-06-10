#include <gtest/gtest.h>

#include "earth_engine/interaction/InputManager.h"

#include <vector>

using namespace earth_engine;

namespace {

InputEvent touch(InputEvent::Type type, float x, float y, double timestamp) {
    InputEvent event;
    event.type = type;
    event.screenX = x;
    event.screenY = y;
    event.pointerType = InputEvent::PointerType::Touch;
    event.timestamp = timestamp;
    return event;
}

InputEvent pinch(InputEvent::Type type, float scale, double timestamp) {
    InputEvent event;
    event.type = type;
    event.pinchScale = scale;
    event.pointerType = InputEvent::PointerType::Touch;
    event.timestamp = timestamp;
    return event;
}

} // namespace

TEST(InputManagerTest, DragThresholdSeparatesClickFromDrag) {
    InputManager manager;
    std::vector<InputManager::Gesture> gestures;
    manager.setCallback([&](InputManager::Gesture gesture, const InputEvent&) {
        gestures.push_back(gesture);
    });

    manager.process(touch(InputEvent::Type::PointerDown, 100.0f, 100.0f, 1.0));
    manager.process(touch(InputEvent::Type::PointerMove, 103.0f, 104.0f, 1.1));
    manager.process(touch(InputEvent::Type::PointerUp, 103.0f, 104.0f, 1.2));

    ASSERT_EQ(1u, gestures.size());
    EXPECT_EQ(InputManager::Gesture::Click, gestures[0]);
}

TEST(InputManagerTest, PinchInterruptsSinglePointerTracking) {
    InputManager manager;
    std::vector<InputManager::Gesture> gestures;
    std::vector<float> scales;
    manager.setCallback([&](InputManager::Gesture gesture, const InputEvent& event) {
        gestures.push_back(gesture);
        scales.push_back(event.pinchScale);
    });

    manager.process(touch(InputEvent::Type::PointerDown, 100.0f, 100.0f, 1.0));
    manager.process(pinch(InputEvent::Type::PinchStart, 1.0f, 1.1));
    manager.process(pinch(InputEvent::Type::PinchMove, 1.6f, 1.2));
    manager.process(pinch(InputEvent::Type::PinchEnd, 1.0f, 1.3));
    manager.process(touch(InputEvent::Type::PointerUp, 100.0f, 100.0f, 1.4));

    ASSERT_EQ(3u, gestures.size());
    EXPECT_EQ(InputManager::Gesture::PinchStart, gestures[0]);
    EXPECT_EQ(InputManager::Gesture::PinchMove, gestures[1]);
    EXPECT_EQ(InputManager::Gesture::PinchEnd, gestures[2]);
    EXPECT_FLOAT_EQ(1.6f, scales[1]);
}

TEST(InputManagerTest, CancelClearsDragWithoutClick) {
    InputManager manager;
    std::vector<InputManager::Gesture> gestures;
    manager.setCallback([&](InputManager::Gesture gesture, const InputEvent&) {
        gestures.push_back(gesture);
    });

    manager.process(touch(InputEvent::Type::PointerDown, 100.0f, 100.0f, 1.0));
    manager.process(touch(InputEvent::Type::PointerMove, 120.0f, 100.0f, 1.1));
    manager.process(touch(InputEvent::Type::Cancel, 120.0f, 100.0f, 1.2));
    manager.process(touch(InputEvent::Type::PointerUp, 120.0f, 100.0f, 1.3));

    ASSERT_EQ(3u, gestures.size());
    EXPECT_EQ(InputManager::Gesture::DragStart, gestures[0]);
    EXPECT_EQ(InputManager::Gesture::DragMove, gestures[1]);
    EXPECT_EQ(InputManager::Gesture::DragEnd, gestures[2]);
}

TEST(InputManagerTest, PinchEndSuppressesFollowingPointerUpClick) {
    InputManager manager;
    std::vector<InputManager::Gesture> gestures;
    manager.setCallback([&](InputManager::Gesture gesture, const InputEvent&) {
        gestures.push_back(gesture);
    });

    manager.process(touch(InputEvent::Type::PointerDown, 100.0f, 100.0f, 1.0));
    manager.process(pinch(InputEvent::Type::PinchStart, 1.0f, 1.1));
    manager.process(pinch(InputEvent::Type::PinchEnd, 1.0f, 1.2));
    manager.process(touch(InputEvent::Type::PointerUp, 100.0f, 100.0f, 1.3));

    ASSERT_EQ(2u, gestures.size());
    EXPECT_EQ(InputManager::Gesture::PinchStart, gestures[0]);
    EXPECT_EQ(InputManager::Gesture::PinchEnd, gestures[1]);
}

TEST(InputManagerTest, PinchMoveWithoutStartSynthesizesCleanStart) {
    InputManager manager;
    std::vector<InputManager::Gesture> gestures;
    std::vector<float> scales;
    manager.setCallback([&](InputManager::Gesture gesture, const InputEvent& event) {
        gestures.push_back(gesture);
        scales.push_back(event.pinchScale);
    });

    manager.process(pinch(InputEvent::Type::PinchMove, 1.4f, 1.0));
    manager.process(pinch(InputEvent::Type::PinchEnd, 1.0f, 1.1));

    ASSERT_EQ(3u, gestures.size());
    EXPECT_EQ(InputManager::Gesture::PinchStart, gestures[0]);
    EXPECT_EQ(InputManager::Gesture::PinchMove, gestures[1]);
    EXPECT_EQ(InputManager::Gesture::PinchEnd, gestures[2]);
    EXPECT_FLOAT_EQ(1.0f, scales[0]);
    EXPECT_FLOAT_EQ(1.4f, scales[1]);
}

TEST(InputManagerTest, CancelClearsPinchWithoutClick) {
    InputManager manager;
    std::vector<InputManager::Gesture> gestures;
    manager.setCallback([&](InputManager::Gesture gesture, const InputEvent&) {
        gestures.push_back(gesture);
    });

    manager.process(pinch(InputEvent::Type::PinchStart, 1.0f, 1.0));
    manager.process(pinch(InputEvent::Type::PinchMove, 1.2f, 1.1));
    manager.process(touch(InputEvent::Type::Cancel, 120.0f, 120.0f, 1.2));
    manager.process(touch(InputEvent::Type::PointerUp, 120.0f, 120.0f, 1.3));

    ASSERT_EQ(3u, gestures.size());
    EXPECT_EQ(InputManager::Gesture::PinchStart, gestures[0]);
    EXPECT_EQ(InputManager::Gesture::PinchMove, gestures[1]);
    EXPECT_EQ(InputManager::Gesture::PinchEnd, gestures[2]);
}
