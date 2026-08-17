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

InputEvent pinchPair(InputEvent::Type type,
                     float p0x, float p0y,
                     float p1x, float p1y,
                     double timestamp) {
    InputEvent event;
    event.type = type;
    event.pointerType = InputEvent::PointerType::Touch;
    event.timestamp = timestamp;
    event.hasPointerPair = true;
    event.pointer0X = p0x;
    event.pointer0Y = p0y;
    event.pointer1X = p1x;
    event.pointer1Y = p1y;
    event.screenX = (p0x + p1x) * 0.5f;
    event.screenY = (p0y + p1y) * 0.5f;
    event.pointerCount = 2;
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
    EXPECT_EQ(InputManager::Gesture::DragCancel, gestures[2]);
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
    EXPECT_EQ(InputManager::Gesture::PinchCancel, gestures[2]);
}

// ---- 双指会话 mode latch（起手快照 + 单次判定，整段手势不再改）----

namespace {

// 收集每个 PinchMove 转发事件的完整副本，便于断言派生量与 mode。
struct PinchEventLog {
    InputManager manager;
    std::vector<InputEvent> moves;
    PinchEventLog() {
        manager.setCallback([this](InputManager::Gesture gesture,
                                   const InputEvent& event) {
            if (gesture == InputManager::Gesture::PinchMove) {
                moves.push_back(event);
            }
        });
    }
};

} // namespace

TEST(InputManagerTest, PinchLatchPitchOnParallelVerticalMove) {
    PinchEventLog log;
    // 首个 pair 事件建立基准；两指平行同向竖移 → Pitch，且横向漂移后保持。
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  300.0f, 300.0f, 500.0f, 300.0f, 1.00));
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  300.0f, 330.0f, 500.0f, 330.0f, 1.02));
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  340.0f, 360.0f, 540.0f, 360.0f, 1.04));

    ASSERT_EQ(3u, log.moves.size());
    EXPECT_EQ(InputEvent::PinchMode::Undecided, log.moves[0].pinchMode);
    EXPECT_EQ(InputEvent::PinchMode::Pitch, log.moves[1].pinchMode);
    // 已 latch：第三个事件质心横移 40px（>8dp 阈值）也不得翻成 Manipulate。
    EXPECT_EQ(InputEvent::PinchMode::Pitch, log.moves[2].pinchMode);
}

TEST(InputManagerTest, PinchCombinationZoomThenPitchBothEngaged) {
    PinchEventLog log;
    // 契约 2.2 组合：两指张开（超过 0.1 log2 阈值）→ 缩放轴激活；随后同向
    // 竖移可再锁定倾斜轴——两轴同时生效，不是互斥 latch。
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  300.0f, 300.0f, 500.0f, 300.0f, 1.00));
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  280.0f, 300.0f, 520.0f, 300.0f, 1.02));
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  280.0f, 340.0f, 520.0f, 340.0f, 1.04));

    ASSERT_EQ(3u, log.moves.size());
    EXPECT_EQ(InputEvent::PinchMode::Manipulate, log.moves[1].pinchMode);
    EXPECT_EQ(InputEvent::PinchMode::Pitch, log.moves[2].pinchMode);
    EXPECT_NEAR(1.2f, log.moves[1].pinchScaleFromStart, 1e-4f);
    EXPECT_TRUE(log.moves[1].pinchZoomEngaged);
    EXPECT_TRUE(log.moves[2].pinchZoomEngaged);  // 缩放轴保持激活
}

TEST(InputManagerTest, PinchZoomEngagesAboveThreshold) {
    PinchEventLog log;
    // 阈值 0.1 log2 ≈ 7.2%：5% 不激活，20% 激活。
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  300.0f, 300.0f, 500.0f, 300.0f, 1.00));
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  295.0f, 300.0f, 505.0f, 300.0f, 1.02));
    ASSERT_EQ(2u, log.moves.size());
    EXPECT_FALSE(log.moves[1].pinchZoomEngaged);
    EXPECT_EQ(InputEvent::PinchMode::Undecided, log.moves[1].pinchMode);

    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  280.0f, 300.0f, 520.0f, 300.0f, 1.04));
    ASSERT_EQ(3u, log.moves.size());
    EXPECT_TRUE(log.moves[2].pinchZoomEngaged);
    EXPECT_EQ(InputEvent::PinchMode::Manipulate, log.moves[2].pinchMode);
}

TEST(InputManagerTest, PinchRotateEngagesAboveArcThreshold) {
    PinchEventLog log;
    // 旋转阈值 = 25px 弧长（|twist|·spread0）。spread0=200：
    // 0.1 rad×200=20px 不激活；0.15 rad×200=30px 激活。
    const float cx = 400.0f, cy = 300.0f, r = 100.0f;
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  cx - r, cy, cx + r, cy, 1.00));
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  cx - r * std::cos(0.1), cy - r * std::sin(0.1),
                                  cx + r * std::cos(0.1), cy + r * std::sin(0.1),
                                  1.02));
    EXPECT_FALSE(log.moves.back().pinchRotateEngaged);

    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  cx - r * std::cos(0.15), cy - r * std::sin(0.15),
                                  cx + r * std::cos(0.15), cy + r * std::sin(0.15),
                                  1.04));
    EXPECT_TRUE(log.moves.back().pinchRotateEngaged);
    EXPECT_EQ(InputEvent::PinchMode::Manipulate, log.moves.back().pinchMode);
}

TEST(InputManagerTest, PinchPitchRejectedWhenSecondFingerLate) {
    PinchEventLog log;
    // 一指先动、另一指 100ms 内未跟上（Mapbox ALLOWED_SINGLE_TOUCH_TIME）→
    // 倾斜锁永久拒绝；此后两指同向竖移也不再触发 pitch。
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  300.0f, 300.0f, 500.0f, 300.0f, 1.00));
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  300.0f, 305.0f, 500.0f, 300.0f, 1.02));
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  300.0f, 310.0f, 500.0f, 300.0f, 1.20));
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  300.0f, 320.0f, 500.0f, 320.0f, 1.22));
    EXPECT_EQ(InputEvent::PinchMode::Manipulate, log.moves.back().pinchMode);
}

TEST(InputManagerTest, PinchLatchManipulateOnHorizontalCentroidMove) {
    PinchEventLog log;
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  300.0f, 300.0f, 500.0f, 300.0f, 1.00));
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  330.0f, 300.0f, 530.0f, 300.0f, 1.02));

    ASSERT_EQ(2u, log.moves.size());
    EXPECT_EQ(InputEvent::PinchMode::Manipulate, log.moves[1].pinchMode);
}

TEST(InputManagerTest, PinchLatchManipulateOnOppositeVerticalMove) {
    PinchEventLog log;
    // 两指竖向反向（拧动）→ 连线角变化触发 Manipulate，绝不误判 Pitch。
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  300.0f, 300.0f, 500.0f, 300.0f, 1.00));
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  300.0f, 330.0f, 500.0f, 270.0f, 1.02));

    ASSERT_EQ(2u, log.moves.size());
    EXPECT_EQ(InputEvent::PinchMode::Manipulate, log.moves[1].pinchMode);
}

TEST(InputManagerTest, PinchLatchStableUnderThresholdNoise) {
    PinchEventLog log;
    // N5：Pitch latch 后，围绕阈值抖动的事件流（spread 抖动 ±8%、质心横移）
    // 不得引起任何一次模式翻转——旧实现每事件重判会在这里疯狂抖动。
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  300.0f, 300.0f, 500.0f, 300.0f, 1.00));
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  300.0f, 330.0f, 500.0f, 330.0f, 1.02));
    ASSERT_EQ(InputEvent::PinchMode::Pitch, log.moves.back().pinchMode);

    float y = 330.0f;
    for (int i = 0; i < 20; ++i) {
        const float wobble = (i % 2 == 0) ? 16.0f : -16.0f;  // spread ±8%
        const float driftX = static_cast<float>(i) * 3.0f;
        y += 4.0f;
        log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                      300.0f - wobble + driftX, y,
                                      500.0f + wobble + driftX, y,
                                      1.04 + 0.02 * i));
    }
    for (const InputEvent& e : log.moves) {
        if (&e == &log.moves.front()) continue;  // 基准帧 Undecided
        EXPECT_EQ(InputEvent::PinchMode::Pitch, e.pinchMode);
    }
}

TEST(InputManagerTest, PinchLatchTimeoutFallsBackToManipulate) {
    PinchEventLog log;
    // 两指几乎不动超过 150ms → 兜底 Manipulate（错判偏向"能平移"侧）。
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  300.0f, 300.0f, 500.0f, 300.0f, 1.00));
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  300.5f, 300.5f, 500.5f, 300.5f, 1.05));
    EXPECT_EQ(InputEvent::PinchMode::Undecided, log.moves.back().pinchMode);
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  300.5f, 300.5f, 500.5f, 300.5f, 1.20));
    EXPECT_EQ(InputEvent::PinchMode::Manipulate, log.moves.back().pinchMode);
}

TEST(InputManagerTest, PinchWithoutPointerPairDefaultsToManipulate) {
    PinchEventLog log;
    // 平台没给两指坐标（如 iOS 迁移前）：安全默认 Manipulate + 派生量恒 1/0。
    log.manager.process(pinch(InputEvent::Type::PinchStart, 1.0f, 1.0));
    log.manager.process(pinch(InputEvent::Type::PinchMove, 1.3f, 1.1));

    ASSERT_EQ(1u, log.moves.size());
    EXPECT_EQ(InputEvent::PinchMode::Manipulate, log.moves[0].pinchMode);
    EXPECT_FLOAT_EQ(1.0f, log.moves[0].pinchScaleFromStart);
    EXPECT_FLOAT_EQ(0.0f, log.moves[0].twistFromStartRadians);
    EXPECT_FLOAT_EQ(1.3f, log.moves[0].pinchScale);  // 旧派生量原样透传
}

TEST(InputManagerTest, PinchDerivedTwistAccumulatesFromStart) {
    PinchEventLog log;
    // 两指绕质心逐步拧动：twistFromStartRadians 应为相对起手的累计角。
    const float cx = 400.0f, cy = 300.0f, r = 100.0f;
    double t = 1.0;
    float lastTwist = 0.0f;
    log.manager.process(pinchPair(InputEvent::Type::PinchMove,
                                  cx - r, cy, cx + r, cy, t));
    for (int i = 1; i <= 6; ++i) {
        const float a = 0.1f * static_cast<float>(i);
        t += 0.02;
        log.manager.process(pinchPair(
            InputEvent::Type::PinchMove,
            cx - r * std::cos(a), cy - r * std::sin(a),
            cx + r * std::cos(a), cy + r * std::sin(a), t));
        lastTwist = log.moves.back().twistFromStartRadians;
    }
    EXPECT_NEAR(0.6f, std::abs(lastTwist), 1e-3f);
}
