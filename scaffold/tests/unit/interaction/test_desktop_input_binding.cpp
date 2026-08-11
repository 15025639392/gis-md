#include <gtest/gtest.h>

#include "earth_engine/interaction/InputEvent.h"
#include "earth_engine/interaction/InputManager.h"

#include <cmath>
#include <vector>

using namespace earth_engine;

namespace {

struct Emitted {
    InputManager::Gesture gesture;
    InputEvent event;
};

class Recorder {
public:
    void attach(InputManager& manager) {
        manager.setCallback([this](InputManager::Gesture g, const InputEvent& e) {
            log.push_back(Emitted{g, e});
        });
    }
    std::vector<Emitted> log;

    std::vector<Emitted> ofType(InputManager::Gesture g) const {
        std::vector<Emitted> out;
        for (const Emitted& e : log) {
            if (e.gesture == g) out.push_back(e);
        }
        return out;
    }
    void clear() { log.clear(); }

    /// ⚠️ 用它而不是 `ofType(g).back()`:空集合上的 `.back()` 是 UB,读到垃圾后
    /// 断言会报"值不对"而不是"根本没发出这个手势",归因方向完全错。
    const InputEvent& last(InputManager::Gesture g) const {
        const std::vector<Emitted> v = ofType(g);
        EXPECT_FALSE(v.empty()) << "期望的手势一条都没发出来";
        static InputEvent fallback;
        return v.empty() ? fallback : (lastCache = v.back()).event;
    }
    mutable Emitted lastCache{};
};

InputEvent mouse(InputEvent::Type type, float x, float y, int buttons,
                 double t) {
    InputEvent e;
    e.type = type;
    e.pointerType = InputEvent::PointerType::Mouse;
    e.screenX = x;
    e.screenY = y;
    e.buttons = buttons;
    e.timestamp = t;
    return e;
}

InputEvent wheel(float x, float y, float delta, double t) {
    InputEvent e;
    e.type = InputEvent::Type::Wheel;
    e.pointerType = InputEvent::PointerType::Mouse;
    e.screenX = x;
    e.screenY = y;
    e.wheelDelta = delta;
    e.timestamp = t;
    return e;
}

/// 真实双指捏合(带 pointer pair),用作"同一通道"的参照物。
InputEvent twoFinger(InputEvent::Type type, float cx, float cy, float spread,
                     double t) {
    InputEvent e;
    e.type = type;
    e.pointerType = InputEvent::PointerType::Touch;
    e.pointerCount = 2;
    e.hasPointerPair = true;
    e.pointer0X = cx - spread * 0.5f;
    e.pointer0Y = cy;
    e.pointer1X = cx + spread * 0.5f;
    e.pointer1Y = cy;
    e.timestamp = t;
    return e;
}

constexpr int kLeft = 1;
constexpr int kRight = 2;
constexpr int kMiddle = 4;

}  // namespace

// ============================================================
// 绑定表本身
// ============================================================

TEST(DesktopBindingTest, DefaultTableMatchesTheDocumentedBindings) {
    InputManager m;
    InputEvent::Modifiers none;
    InputEvent::Modifiers ctrl;
    ctrl.ctrl = true;

    EXPECT_EQ(InputManager::DesktopAction::AnchorDrag,
              m.resolveDesktopAction(InputManager::DesktopTrigger::LeftDrag,
                                     none));
    EXPECT_EQ(InputManager::DesktopAction::Zoom,
              m.resolveDesktopAction(InputManager::DesktopTrigger::Wheel, none));
    EXPECT_EQ(InputManager::DesktopAction::Tilt,
              m.resolveDesktopAction(InputManager::DesktopTrigger::MiddleDrag,
                                     none));
    EXPECT_EQ(InputManager::DesktopAction::Zoom,
              m.resolveDesktopAction(InputManager::DesktopTrigger::RightDrag,
                                     none));
    EXPECT_EQ(InputManager::DesktopAction::Tilt,
              m.resolveDesktopAction(InputManager::DesktopTrigger::LeftDrag,
                                     ctrl));
}

TEST(DesktopBindingTest, ModifierMatchIsExactNotSubset) {
    InputManager m;
    InputEvent::Modifiers ctrlShift;
    ctrlShift.ctrl = true;
    ctrlShift.shift = true;

    // Ctrl+Shift+左键**不该**命中 "Ctrl+左键 = tilt",也不该命中 "无修饰 = 拖拽"。
    // 包含式匹配会让这两条同时命中,而哪条先命中取决于表的顺序 —— 那种"看起来
    // 能用、换个顺序就变"的行为最难查。
    EXPECT_EQ(InputManager::DesktopAction::None,
              m.resolveDesktopAction(InputManager::DesktopTrigger::LeftDrag,
                                     ctrlShift));
}

TEST(DesktopBindingTest, TableIsReplaceable) {
    InputManager m;
    InputEvent::Modifiers none;
    m.setDesktopBindings({{InputManager::DesktopTrigger::MiddleDrag, none,
                           InputManager::DesktopAction::Zoom}});
    EXPECT_EQ(InputManager::DesktopAction::Zoom,
              m.resolveDesktopAction(InputManager::DesktopTrigger::MiddleDrag,
                                     none));
    // 表被整体替换 ⇒ 原有绑定消失(不是合并)。
    EXPECT_EQ(InputManager::DesktopAction::None,
              m.resolveDesktopAction(InputManager::DesktopTrigger::Wheel, none));
}

// ============================================================
// 判据 ①:滚轮 zoom 与双指 zoom 走同一锚点通道
// ============================================================

TEST(DesktopBindingTest, WheelEmitsTheSamePinchContractAsTwoFingerZoom) {
    InputManager m;
    Recorder rec;
    rec.attach(m);

    // 真双指:捏开到 1.25×,质心 (400,300)。
    m.process(twoFinger(InputEvent::Type::PinchStart, 400.0f, 300.0f, 200.0f, 0.0));
    m.process(twoFinger(InputEvent::Type::PinchMove, 400.0f, 300.0f, 250.0f, 0.016));
    const std::vector<Emitted> touchMoves =
        rec.ofType(InputManager::Gesture::PinchMove);
    ASSERT_FALSE(touchMoves.empty());
    const InputEvent& touch = touchMoves.back().event;

    rec.clear();
    m.reset();
    m.process(wheel(400.0f, 300.0f, 1.0f, 1.0));
    const std::vector<Emitted> wheelMoves =
        rec.ofType(InputManager::Gesture::PinchMove);
    ASSERT_EQ(1u, wheelMoves.size());
    const InputEvent& synth = wheelMoves.back().event;

    // **同一份契约**:下游 SceneInputCoordinator 只读这几个字段,读不出差别 ⇒
    // 相机层走的是同一段锚点钉合代码,而不是"另写一套滚轮缩放碰巧也对"。
    EXPECT_TRUE(synth.hasPointerPair) << "不带 pointer pair 会掉进旧契约适配器";
    EXPECT_EQ(touch.pinchMode, synth.pinchMode);
    EXPECT_EQ(InputEvent::PinchMode::Manipulate, synth.pinchMode);
    EXPECT_FLOAT_EQ(400.0f, (synth.pointer0X + synth.pointer1X) * 0.5f);
    EXPECT_FLOAT_EQ(300.0f, (synth.pointer0Y + synth.pointer1Y) * 0.5f);
    EXPECT_FLOAT_EQ(0.0f, synth.twistFromStartRadians);
    EXPECT_GT(synth.pinchScaleFromStart, 1.0f) << "正 delta 应拉近";
}

TEST(DesktopBindingTest, WheelIsACompleteMicroSessionPerNotch) {
    InputManager m;
    Recorder rec;
    rec.attach(m);
    m.process(wheel(400.0f, 300.0f, 1.0f, 1.0));

    // 每格 = Start→Move→End 一整段。这样每格都在光标处重取锚点(朝光标缩放),
    // 且 Start 与 Move 时间戳相同 ⇒ dt=0 ⇒ **不种 zoom 惯性**,滚轮是干脆的
    // 离散步进而不是甩飞。会话不收尾的话 pinching_ 会一直挂着,挡住后续拖拽。
    ASSERT_EQ(3u, rec.log.size());
    EXPECT_EQ(InputManager::Gesture::PinchStart, rec.log[0].gesture);
    EXPECT_EQ(InputManager::Gesture::PinchMove, rec.log[1].gesture);
    EXPECT_EQ(InputManager::Gesture::PinchEnd, rec.log[2].gesture);
    EXPECT_DOUBLE_EQ(rec.log[0].event.timestamp, rec.log[1].event.timestamp);
}

TEST(DesktopBindingTest, WheelDirectionAndMagnitudeAreMonotonic) {
    InputManager m;
    Recorder rec;
    rec.attach(m);

    auto scaleFor = [&](float delta) {
        rec.clear();
        m.process(wheel(400.0f, 300.0f, delta, 1.0));
        return rec.last(InputManager::Gesture::PinchMove).pinchScaleFromStart;
    };
    const float in1 = scaleFor(1.0f);
    const float in2 = scaleFor(2.0f);
    const float out1 = scaleFor(-1.0f);

    EXPECT_GT(in1, 1.0f);
    EXPECT_GT(in2, in1) << "两格应比一格缩得多";
    EXPECT_LT(out1, 1.0f) << "反向滚应拉远";
    // 对数尺度对称:滚进一格再滚出一格应回到原处。
    EXPECT_NEAR(1.0f, in1 * out1, 1e-5f);
}

// ============================================================
// 判据 ②:中键 tilt 与双指 Pitch 同语义
// ============================================================

TEST(DesktopBindingTest, MiddleDragEmitsPitchModeWithCentroidFollowingCursor) {
    InputManager m;
    Recorder rec;
    rec.attach(m);

    m.process(mouse(InputEvent::Type::PointerDown, 400.0f, 300.0f, kMiddle, 0.0));
    m.process(mouse(InputEvent::Type::PointerMove, 400.0f, 260.0f, kMiddle, 0.016));
    m.process(mouse(InputEvent::Type::PointerUp, 400.0f, 260.0f, 0, 0.032));

    const std::vector<Emitted> moves =
        rec.ofType(InputManager::Gesture::PinchMove);
    ASSERT_EQ(1u, moves.size());
    const InputEvent& e = moves.back().event;

    // 与双指 Pitch **逐字同语义**:mode=Pitch + 质心跟随 ⇒ 相机层那段"质心 Y
    // 相对基线绝对映射 pitch"的代码原样生效,不需要为鼠标写第二份。
    EXPECT_EQ(InputEvent::PinchMode::Pitch, e.pinchMode);
    EXPECT_TRUE(e.hasPointerPair);
    EXPECT_FLOAT_EQ(260.0f, (e.pointer0Y + e.pointer1Y) * 0.5f);
    EXPECT_FLOAT_EQ(1.0f, e.pinchScaleFromStart) << "tilt 不该缩放";

    // 起手与收尾各一次,且都是 Pitch —— 收尾 mode 写错会让相机层按 Manipulate
    // 收官,残留一次锚点钉合。
    ASSERT_EQ(1u, rec.ofType(InputManager::Gesture::PinchStart).size());
    ASSERT_EQ(1u, rec.ofType(InputManager::Gesture::PinchEnd).size());
    EXPECT_EQ(InputEvent::PinchMode::Pitch,
              rec.last(InputManager::Gesture::PinchEnd).pinchMode);
}

TEST(DesktopBindingTest, CtrlLeftDragTiltsIdenticallyToMiddleDrag) {
    auto run = [](int buttons, bool ctrl) {
        InputManager m;
        Recorder rec;
        rec.attach(m);
        InputEvent down = mouse(InputEvent::Type::PointerDown, 400.0f, 300.0f,
                                buttons, 0.0);
        down.modifiers.ctrl = ctrl;
        InputEvent move = mouse(InputEvent::Type::PointerMove, 400.0f, 250.0f,
                                buttons, 0.016);
        move.modifiers.ctrl = ctrl;
        m.process(down);
        m.process(move);
        return rec.last(InputManager::Gesture::PinchMove);
    };
    const InputEvent middle = run(kMiddle, false);
    const InputEvent ctrlLeft = run(kLeft, true);

    // 两条绑定必须产出**逐字段相同**的合成事件,否则"两种 tilt 手感不一样"
    // 这种问题只能靠人肉对比发现。
    EXPECT_EQ(middle.pinchMode, ctrlLeft.pinchMode);
    EXPECT_FLOAT_EQ(middle.pinchScaleFromStart, ctrlLeft.pinchScaleFromStart);
    EXPECT_FLOAT_EQ((middle.pointer0X + middle.pointer1X) * 0.5f,
                    (ctrlLeft.pointer0X + ctrlLeft.pointer1X) * 0.5f);
    EXPECT_FLOAT_EQ((middle.pointer0Y + middle.pointer1Y) * 0.5f,
                    (ctrlLeft.pointer0Y + ctrlLeft.pointer1Y) * 0.5f);
}

// ============================================================
// 通道隔离:桌面绑定不能漏到触摸路径
// ============================================================

TEST(DesktopBindingTest, PlainLeftDragStillGoesThroughTheAnchorDragChannel) {
    InputManager m;
    Recorder rec;
    rec.attach(m);

    m.process(mouse(InputEvent::Type::PointerDown, 400.0f, 300.0f, kLeft, 0.0));
    m.process(mouse(InputEvent::Type::PointerMove, 450.0f, 300.0f, kLeft, 0.016));
    m.process(mouse(InputEvent::Type::PointerUp, 450.0f, 300.0f, 0, 0.032));

    // 无修饰左键拖 = 锚点拖拽(与单指触摸同一条路),**不该**产出任何 pinch。
    EXPECT_FALSE(rec.ofType(InputManager::Gesture::DragMove).empty());
    EXPECT_TRUE(rec.ofType(InputManager::Gesture::PinchMove).empty());
}

TEST(DesktopBindingTest, MiddleDragDoesNotAlsoProduceAnchorDrag) {
    InputManager m;
    Recorder rec;
    rec.attach(m);

    m.process(mouse(InputEvent::Type::PointerDown, 400.0f, 300.0f, kMiddle, 0.0));
    m.process(mouse(InputEvent::Type::PointerMove, 460.0f, 250.0f, kMiddle, 0.016));

    // 桌面通道必须**整条消费**事件。漏下去的话中键拖会同时 tilt 和拖地球,
    // 画面上表现为"倾斜时地面还在跑",极难归因到输入层。
    EXPECT_TRUE(rec.ofType(InputManager::Gesture::DragStart).empty());
    EXPECT_TRUE(rec.ofType(InputManager::Gesture::DragMove).empty());
}

TEST(DesktopBindingTest, RightDragZoomsWithoutMovingTheAnchor) {
    InputManager m;
    Recorder rec;
    rec.attach(m);

    m.process(mouse(InputEvent::Type::PointerDown, 400.0f, 300.0f, kRight, 0.0));
    m.process(mouse(InputEvent::Type::PointerMove, 480.0f, 240.0f, kRight, 0.016));
    const InputEvent& e =
        rec.last(InputManager::Gesture::PinchMove);

    // 质心**钉在起手处**不跟光标:跟了就会触发 pin 的横向世界运动(拖着地球跑),
    // 而右键拖的语义只有缩放。
    EXPECT_FLOAT_EQ(400.0f, (e.pointer0X + e.pointer1X) * 0.5f);
    EXPECT_FLOAT_EQ(300.0f, (e.pointer0Y + e.pointer1Y) * 0.5f);
    EXPECT_GT(e.pinchScaleFromStart, 1.0f) << "上拖应拉近";
    EXPECT_EQ(InputEvent::PinchMode::Manipulate, e.pinchMode);
}

TEST(DesktopBindingTest, TouchEventsAreUnaffectedByDesktopBindings) {
    InputManager m;
    Recorder rec;
    rec.attach(m);

    // 触摸事件的 buttons 常为 0/1,不能被桌面表误判。
    InputEvent down = mouse(InputEvent::Type::PointerDown, 400.0f, 300.0f,
                            kMiddle, 0.0);
    down.pointerType = InputEvent::PointerType::Touch;
    m.process(down);
    EXPECT_TRUE(rec.ofType(InputManager::Gesture::PinchStart).empty())
        << "触摸不该走桌面通道";
}

TEST(DesktopBindingTest, CancelClearsTheDesktopSession) {
    InputManager m;
    Recorder rec;
    rec.attach(m);
    m.process(mouse(InputEvent::Type::PointerDown, 400.0f, 300.0f, kMiddle, 0.0));
    InputEvent cancel;
    cancel.type = InputEvent::Type::Cancel;
    m.process(cancel);
    rec.clear();

    // 取消后残留的会话会把后续 move 继续当成 tilt(鼠标已经松开了)。
    m.process(mouse(InputEvent::Type::PointerMove, 460.0f, 250.0f, 0, 0.05));
    EXPECT_TRUE(rec.ofType(InputManager::Gesture::PinchMove).empty());
}
