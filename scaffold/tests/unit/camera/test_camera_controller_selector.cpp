#include <gtest/gtest.h>

#include "earth_engine/camera/CameraControllerSelector.h"
#include "earth_engine/camera/controllers/ICameraController.h"

#include <memory>
#include <string>
#include <vector>

using namespace earth_engine;

namespace {

/// 记录型假控制器。它存在的意义不只是"给 selector 一个被切换的对象"——
/// **它是 `ICameraController` 的第二个实现**。只有一个实现的接口是猜测,
/// 第二个实现才能证明契约里没有偷偷夹带 FreeGlobeController 的私有假设。
class FakeController final : public ICameraController {
public:
    explicit FakeController(std::vector<std::string>* log, std::string name)
        : log_(log), name_(std::move(name)) {}

    void onActivate() override { log_->push_back(name_ + ":activate"); }
    void onDeactivate() override { log_->push_back(name_ + ":deactivate"); }
    void tick(double deltaSeconds) override {
        tickCount_++;
        lastDelta_ = deltaSeconds;
    }
    bool isAnimating() const override { return animating_; }
    void setViewport(int widthPixels, int heightPixels) override {
        viewportWidth_ = widthPixels;
        viewportHeight_ = heightPixels;
    }

    void setAnimating(bool animating) { animating_ = animating; }

    int tickCount() const { return tickCount_; }
    double lastDelta() const { return lastDelta_; }
    int viewportWidth() const { return viewportWidth_; }
    int viewportHeight() const { return viewportHeight_; }

private:
    std::vector<std::string>* log_;
    std::string name_;
    int tickCount_ = 0;
    double lastDelta_ = -1.0;
    bool animating_ = false;
    int viewportWidth_ = 0;
    int viewportHeight_ = 0;
};

/// 与 FakeController 无继承关系的第三个实现,用来验 activeAs<T>/findOfType<T>
/// 确实按具体类型区分,而不是"随便返回第一个"。
class OtherController final : public ICameraController {
public:
    void tick(double) override {}
    bool isAnimating() const override { return false; }
};

}  // namespace

TEST(CameraControllerSelectorTest, FirstRegisteredBecomesActiveAndGetsActivate) {
    std::vector<std::string> log;
    CameraControllerSelector selector;
    EXPECT_EQ(nullptr, selector.active());

    selector.add("a", std::make_unique<FakeController>(&log, "a"));

    EXPECT_EQ("a", selector.activeName());
    ASSERT_NE(nullptr, selector.active());
    // 首个注册者立即激活:否则存在"构造完还没选就 tick"的半初始化窗口。
    EXPECT_EQ(std::vector<std::string>{"a:activate"}, log);
}

TEST(CameraControllerSelectorTest, LaterRegistrationsDoNotStealActive) {
    std::vector<std::string> log;
    CameraControllerSelector selector;
    selector.add("a", std::make_unique<FakeController>(&log, "a"));
    log.clear();

    selector.add("b", std::make_unique<FakeController>(&log, "b"));

    EXPECT_EQ("a", selector.activeName());
    EXPECT_TRUE(log.empty()) << "注册不该产生生命周期回调,只有 select 才该";
}

TEST(CameraControllerSelectorTest, SwitchOrderIsDeactivateThenActivate) {
    std::vector<std::string> log;
    CameraControllerSelector selector;
    selector.add("a", std::make_unique<FakeController>(&log, "a"));
    selector.add("b", std::make_unique<FakeController>(&log, "b"));
    log.clear();

    EXPECT_TRUE(selector.select("b"));

    // 顺序是契约:先让旧的交出瞬时状态,新的才从当前位姿对齐 ⇒ 零跳变。
    const std::vector<std::string> expected{"a:deactivate", "b:activate"};
    EXPECT_EQ(expected, log);
    EXPECT_EQ("b", selector.activeName());
}

TEST(CameraControllerSelectorTest, ReselectingActiveIsNoOp) {
    std::vector<std::string> log;
    CameraControllerSelector selector;
    selector.add("a", std::make_unique<FakeController>(&log, "a"));
    log.clear();

    EXPECT_FALSE(selector.select("a"));
    EXPECT_TRUE(log.empty()) << "重选当前活动者若也走一遍 deactivate/activate,"
                                "惯性会被无声清掉";
}

TEST(CameraControllerSelectorTest, UnknownNameKeepsCurrentActive) {
    std::vector<std::string> log;
    CameraControllerSelector selector;
    selector.add("a", std::make_unique<FakeController>(&log, "a"));
    log.clear();

    EXPECT_FALSE(selector.select("nope"));

    // 关键不变量:未知名字不能留下"没人在驱动"的空档——那会让 tick 静默丢失。
    EXPECT_EQ("a", selector.activeName());
    ASSERT_NE(nullptr, selector.active());
    EXPECT_TRUE(log.empty());
}

TEST(CameraControllerSelectorTest, OnlyActiveControllerIsTicked) {
    std::vector<std::string> log;
    CameraControllerSelector selector;
    selector.add("a", std::make_unique<FakeController>(&log, "a"));
    selector.add("b", std::make_unique<FakeController>(&log, "b"));

    auto* a = static_cast<FakeController*>(selector.active());
    selector.active()->tick(0.5);
    selector.select("b");
    auto* b = static_cast<FakeController*>(selector.active());
    selector.active()->tick(0.25);

    EXPECT_EQ(1, a->tickCount());
    EXPECT_DOUBLE_EQ(0.5, a->lastDelta());
    EXPECT_EQ(1, b->tickCount());
    EXPECT_DOUBLE_EQ(0.25, b->lastDelta());
}

TEST(CameraControllerSelectorTest, ViewportBroadcastsToInactiveControllers) {
    std::vector<std::string> log;
    CameraControllerSelector selector;
    selector.add("a", std::make_unique<FakeController>(&log, "a"));
    selector.add("b", std::make_unique<FakeController>(&log, "b"));
    auto* a = static_cast<FakeController*>(selector.active());
    selector.select("b");
    auto* b = static_cast<FakeController*>(selector.active());

    selector.setViewport(1080, 1920);

    // 广播而非只发给活动者:视口是渲染表面的属性,与"谁在驱动"无关。只发活动者
    // 的话,切换瞬间接管者还揣着旧视口 ⇒ 像素→角度增益用错,第一下手势就跳。
    EXPECT_EQ(1080, a->viewportWidth());
    EXPECT_EQ(1920, a->viewportHeight());
    EXPECT_EQ(1080, b->viewportWidth());
    EXPECT_EQ(1920, b->viewportHeight());
}

TEST(CameraControllerSelectorTest, ActiveAsDiscriminatesByConcreteType) {
    std::vector<std::string> log;
    CameraControllerSelector selector;
    selector.add("fake", std::make_unique<FakeController>(&log, "fake"));
    selector.add("other", std::make_unique<OtherController>());

    // 活动者是 FakeController ⇒ 只有它这一路取得出来。
    EXPECT_NE(nullptr, selector.activeAs<FakeController>());
    EXPECT_EQ(nullptr, selector.activeAs<OtherController>())
        << "输入路由靠它判「当前驱动者吃不吃这种输入」,认错类型 = 事件喂错人";

    EXPECT_TRUE(selector.select("other"));
    EXPECT_EQ(nullptr, selector.activeAs<FakeController>());
    EXPECT_NE(nullptr, selector.activeAs<OtherController>());
}

TEST(CameraControllerSelectorTest, FindOfTypeIgnoresWhoIsActive) {
    std::vector<std::string> log;
    CameraControllerSelector selector;
    selector.add("fake", std::make_unique<FakeController>(&log, "fake"));
    selector.add("other", std::make_unique<OtherController>());
    selector.select("other");

    // 装配期接线(注入 surface picker 等)要能找到未激活的控制器。
    EXPECT_NE(nullptr, selector.findOfType<FakeController>());
    EXPECT_NE(nullptr, selector.findOfType<OtherController>());
}

TEST(CameraControllerSelectorTest, IsAnimatingReadsActiveControllerOnly) {
    std::vector<std::string> log;
    CameraControllerSelector selector;
    selector.add("a", std::make_unique<FakeController>(&log, "a"));
    selector.add("b", std::make_unique<FakeController>(&log, "b"));
    auto* a = static_cast<FakeController*>(selector.active());
    a->setAnimating(true);
    EXPECT_TRUE(selector.active()->isAnimating());

    selector.select("b");

    // 未激活者的滑行状态不该让画面继续重绘——按需渲染据此判"还得画几帧"。
    EXPECT_FALSE(selector.active()->isAnimating());
}
