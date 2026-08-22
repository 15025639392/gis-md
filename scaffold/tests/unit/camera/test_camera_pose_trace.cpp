// 相机位姿逐帧轨迹对拍台(阶段 1 拆分的安全网)。
//
// 目的:把"纯拆分,零行为变更"这句话变成机器可查的判据。每个场景驱动一段
// 确定性输入,逐帧把 (eye, direction, up, groundState) 的**原始位模式**混进
// FNV-1a,断言 hash 等于拆分前录得的常数。浮点重排、运算顺序改变、少调一次
// resolveConstraints——任何一种都会让 hash 变。
//
// ⚠️ 为什么分场景各一个 hash 而不是一个总 hash:总 hash 只能告诉你"变了",
// 分场景能告诉你"哪条路径变了"。拆分期这个定位能力是主要价值。
//
// ⚠️ 场景必须真的走到被拆的路径上。"没出问题"与"没走到那条路"长得一样——
// 每个场景末尾都断言了该路径的机制信号(采样次数/是否钳位/是否有惯性),
// 信号不对说明场景本身失效了,这时 hash 相同毫无意义。

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "earth_engine/camera/CameraSystem.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/scene/Camera.h"

using namespace earth_engine;

namespace {

constexpr double kEarthRadiusMeters = 6378137.0;
constexpr double kDt = 1.0 / 60.0;

// 逐帧位姿 + 地面状态的位精确记录器。
class PoseTrace {
public:
    void record(const Camera& camera,
                const CameraSystem::CameraGroundState& ground) {
        Frame f;
        f.v = {camera.position().x(), camera.position().y(),
               camera.position().z(), camera.direction().x(),
               camera.direction().y(), camera.direction().z(),
               camera.up().x(),        camera.up().y(),
               camera.up().z(),        ground.terrainHeightMeters,
               ground.heightAboveTerrain, ground.nearestGeometryMeters};
        f.flags = static_cast<uint8_t>((ground.valid ? 1u : 0u) |
                                       (ground.hasTerrainData ? 2u : 0u));
        for (double d : f.v) {
            mix(&d, sizeof(d));
        }
        mix(&f.flags, sizeof(f.flags));
        frames_.push_back(f);
    }

    uint64_t hash() const { return hash_; }
    size_t frameCount() const { return frames_.size(); }

    // 逐帧十六进制浮点转储。hash 不等时用它做 diff 定位到具体帧和字段。
    void dump(const char* label) const {
        std::printf("---- trace %s (%zu frames, hash=0x%016llx) ----\n", label,
                    frames_.size(),
                    static_cast<unsigned long long>(hash_));
        for (size_t i = 0; i < frames_.size(); ++i) {
            const Frame& f = frames_[i];
            std::printf("%3zu flags=%u", i, static_cast<unsigned>(f.flags));
            for (double d : f.v) {
                std::printf(" %a", d);
            }
            std::printf("\n");
        }
    }

private:
    struct Frame {
        std::array<double, 12> v{};
        uint8_t flags = 0;
    };

    void mix(const void* data, size_t bytes) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < bytes; ++i) {
            hash_ ^= p[i];
            hash_ *= 1099511628211ull;
        }
    }

    uint64_t hash_ = 14695981039346656037ull;  // FNV-1a offset basis
    std::vector<Frame> frames_;
};

CameraSystem::PinchInput pinchIn(float scaleFromStart, float twistRadians,
                                     float cx, float cy,
                                     CameraSystem::PinchMode mode,
                                     double timestamp) {
    CameraSystem::PinchInput input;
    input.scaleFromStart = scaleFromStart;
    input.twistFromStartRadians = twistRadians;
    input.centroidX = cx;
    input.centroidY = cy;
    input.mode = mode;
    input.timestamp = timestamp;
    return input;
}

// 解析式地形假体(与 test_camera_system 同法):把探针的 ENU 偏移换算回
// 经纬,高度由 heightFn 给出,全点有效。
CameraSystem::TerrainAreaSampleFunc makeAnalyticAreaSampler(
    std::function<double(double, double)> heightFn, int* callCounter) {
    return [heightFn, callCounter](
               const Vec3& groundEcef, double /*radiusMeters*/,
               const std::vector<glm::dvec2>& offsets,
               std::vector<CameraSystem::TerrainSample>& out) {
        if (callCounter) ++(*callCounter);
        const auto& e = Ellipsoid::WGS84();
        const Cartographic c = e.cartesianToCartographic(groundEcef);
        const double cosLat = std::max(std::abs(std::cos(c.latitude())), 0.01);
        out.assign(offsets.size(), {});
        for (size_t i = 0; i < offsets.size(); ++i) {
            const double lat = c.latitude() + offsets[i].y / kEarthRadiusMeters;
            const double lon =
                c.longitude() + offsets[i].x / (kEarthRadiusMeters * cosLat);
            const double h = heightFn(lon, lat);
            out[i].valid = true;
            out[i].heightMeters = h;
            out[i].surfaceEcef =
                e.cartographicToCartesian(Cartographic(lon, lat, h));
        }
    };
}

class PoseTraceTest : public ::testing::Test {
protected:
    void SetUp() override {
        camera_ = std::make_unique<Camera>();
        camera_->setPerspective(glm::radians(60.0), 1.0, 50000000.0);
        controller_ = std::make_unique<CameraSystem>(camera_.get());
        controller_->setViewport(800, 600);
    }

    void step(int frames) {
        for (int i = 0; i < frames; ++i) {
            controller_->update(kDt);
            trace_.record(*camera_, controller_->groundState());
        }
    }

    double altitudeMeters() const {
        return Ellipsoid::WGS84()
            .cartesianToCartographic(camera_->position())
            .height();
    }

    std::unique_ptr<Camera> camera_;
    std::unique_ptr<CameraSystem> controller_;
    PoseTrace trace_;
};

// 断言 hash 并在不等时转储完整轨迹(定位到具体帧/字段)。
#define EXPECT_TRACE_HASH(traceObj, expected)                             \
    do {                                                                  \
        const uint64_t actual = (traceObj).hash();                        \
        if (actual != (expected)) {                                       \
            (traceObj).dump(::testing::UnitTest::GetInstance()            \
                                ->current_test_info()                     \
                                ->name());                                \
            ADD_FAILURE() << "pose trace hash changed: expected 0x"       \
                          << std::hex << (expected) << " actual 0x"       \
                          << actual << std::dec                           \
                          << " (若这是有意的行为变更,更新常数;若本次是纯拆分," \
                             "说明拆坏了——用上面的转储 diff 定位)";        \
        }                                                                 \
    } while (0)

// ---------------------------------------------------------------------------
// 场景 A:单指拖拽良态锚定 + 松手惯性衰减到自清零。
// 覆盖:grabSurfacePoint / solveAnchorRotation 良态分支 / applyCameraRotation /
//       惯性 EMA 累积 / updateInternal 惯性衰减 + 自清零 / 帧末哨兵。
// ---------------------------------------------------------------------------
TEST_F(PoseTraceTest, TraceA_AnchorDragThenInertia) {
    step(1);  // 冷启动帧

    // ① 低空拖拽:走良态锚定路径。⚠️ 2026-08-19 修正:惯性采样改**视角角速度**
    // (手指感知),低空快拖会种下惯性——修复前采相机绕地心角速度,被视差
    // (锚点距/地心距 ~1/4000)压到 ~2e-7 rad,松手第一帧就被 0.5px/帧 判停,
    // "低空拖拽无惯性"是 bug 不是设计。这里 30px/16ms ≈ 1875px/s,视角率
    // ~3.7 rad/s,惯性应启动;随后的 viewDistance 清掉它,② 重新种惯性。
    controller_->onDragStart(400.0f, 300.0f, 0.0);
    trace_.record(*camera_, controller_->groundState());
    for (int i = 1; i <= 8; ++i) {
        controller_->onDragMove(400.0f + 30.0f * i, 300.0f + 15.0f * i,
                                i * 0.016);
        trace_.record(*camera_, controller_->groundState());
    }
    controller_->onDragEnd();
    trace_.record(*camera_, controller_->groundState());
    step(2);

    // ② 高空拖拽:每像素地面尺度大,角速度足够,惯性衰减与自清零才走得到。
    const auto& ell = Ellipsoid::WGS84();
    const Vec3 target =
        ell.cartographicToCartesian(Cartographic::fromDegrees(106.5, 29.6, 0.0));
    controller_->viewDistance(target, 3.0e5);
    step(2);

    controller_->onDragStart(400.0f, 300.0f, 1.0);
    trace_.record(*camera_, controller_->groundState());
    for (int i = 1; i <= 10; ++i) {
        controller_->onDragMove(400.0f + 30.0f * i, 300.0f + 15.0f * i,
                                1.0 + i * 0.016);
        trace_.record(*camera_, controller_->groundState());
    }
    controller_->onDragEnd();
    trace_.record(*camera_, controller_->groundState());

    ASSERT_TRUE(controller_->isSelfAnimating())
        << "场景失效:高空拖拽没有种下惯性,衰减/自清零路径没走到";
    // pan 惯性阻尼 3.0/s,从上限衰减到 1e-4 地板要数秒,固定帧数会不够。
    int guard = 0;
    while (controller_->isSelfAnimating() && guard++ < 600) {
        step(1);
    }
    ASSERT_FALSE(controller_->isSelfAnimating())
        << "场景失效:惯性未自清零,没走到清零分支";

    // 2026-08-19:惯性采样改视角角速度后,① 低空拖拽也会种下惯性并多滑几帧,
    // 轨迹 hash 更新(行为见上方注释)。
    EXPECT_TRACE_HASH(trace_, 0x2a9a122bce4477e5ull);
}

// ---------------------------------------------------------------------------
// 场景 B:双指 dolly + twist + 刚性 pan + Pitch 模式,注入解析地形。
// 覆盖:tryAcquirePinchAnchor / jerk 限幅 / dolly 保锚 / rotateCameraAroundPoint /
//       rotateCameraVerticalAroundPoint(含守卫) / applyPinchPin / 探针路径 /
//       zoom 惯性种入与滑行。
// ---------------------------------------------------------------------------
TEST_F(PoseTraceTest, TraceB_PinchDollyTwistPanPitch) {
    int sampleCalls = 0;
    controller_->setTerrainAreaSampleFunc(makeAnalyticAreaSampler(
        [](double lon, double lat) {
            return 400.0 + 300.0 * std::sin(lon * 900.0) * std::cos(lat * 900.0);
        },
        &sampleCalls));
    step(1);

    double t = 0.0;
    controller_->onPinchGesture(pinchIn(1.0f, 0.0f, 400.0f, 300.0f,
                                        CameraSystem::PinchMode::Undecided,
                                        t));
    trace_.record(*camera_, controller_->groundState());

    // Undecided 窗口:只 dolly/twist,锚点钉起手质心。
    for (int i = 1; i <= 3; ++i) {
        t += 0.016;
        controller_->onPinchGesture(
            pinchIn(1.0f + 0.02f * i, 0.01f * i, 400.0f + i, 300.0f,
                    CameraSystem::PinchMode::Undecided, t));
        trace_.record(*camera_, controller_->groundState());
    }
    // latch 到 Manipulate:刚性 pan 内建,质心移动 = 世界横移。
    for (int i = 1; i <= 8; ++i) {
        t += 0.016;
        controller_->onPinchGesture(
            pinchIn(1.06f + 0.03f * i, 0.03f + 0.012f * i, 400.0f + 5.0f * i,
                    300.0f - 3.0f * i, CameraSystem::PinchMode::Manipulate,
                    t));
        trace_.record(*camera_, controller_->groundState());
    }
    controller_->onPinchEnd();
    trace_.record(*camera_, controller_->groundState());
    step(12);  // zoom + pan 双惯性滑行

    // 独立一段 Pitch:质心竖移驱动俯仰,锚点钉 latch 质心。
    t += 0.5;
    controller_->onPinchGesture(pinchIn(1.0f, 0.0f, 400.0f, 300.0f,
                                        CameraSystem::PinchMode::Undecided,
                                        t));
    trace_.record(*camera_, controller_->groundState());
    for (int i = 1; i <= 10; ++i) {
        t += 0.016;
        controller_->onPinchGesture(
            pinchIn(1.0f, 0.0f, 400.0f, 300.0f - 12.0f * i,
                    CameraSystem::PinchMode::Pitch, t));
        trace_.record(*camera_, controller_->groundState());
    }
    controller_->onPinchEnd();
    trace_.record(*camera_, controller_->groundState());
    step(6);

    EXPECT_GT(sampleCalls, 0) << "场景失效:探针一次都没被调,地形路径没走到";
    EXPECT_TRUE(controller_->groundState().hasTerrainData)
        << "场景失效:没拿到地形样本,滤波/探针分支空转";

    EXPECT_TRACE_HASH(trace_, 0x64b41dda0f3e9395ull);
}

// ---------------------------------------------------------------------------
// 场景 C:低空高山——碰撞钳位 + 保锚退出方向 + 非对称突变滤波。
// 覆盖:constrainEyeAgainstTerrain 钳位分支 / pinnedAnchorWorld 牛顿迭代退出 /
//       updateFilteredTerrainHeight 上升立即与下降指数两条支路 / 代次失效。
// ---------------------------------------------------------------------------
TEST_F(PoseTraceTest, TraceC_TerrainClampAndFilter) {
    // 地形高度可切换(模拟瓦片加载改变数据),配合代次让探针缓存失效。
    double plateauHeight = 200.0;
    uint64_t revision = 1;
    int sampleCalls = 0;
    controller_->setTerrainAreaSampleFunc(makeAnalyticAreaSampler(
        [&plateauHeight](double, double) { return plateauHeight; },
        &sampleCalls));
    controller_->setTerrainRevisionFunc([&revision]() { return revision; });

    const auto& e = Ellipsoid::WGS84();
    const Vec3 target = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.5, 29.6, 0.0));
    controller_->viewDistance(target, 900.0);
    step(2);

    // ① 数据驱动的地形抬升(新瓦片证明脚下是山):滤波应立即生效并把 eye 顶起。
    plateauHeight = 780.0;
    revision = 2;
    step(3);
    EXPECT_GT(controller_->groundState().terrainHeightMeters, 700.0)
        << "场景失效:抬升没被吸收,滤波上升分支没走到";

    // ② 双指压低:走 dolly + 钳位 + 保锚退出。
    double t = 0.0;
    controller_->onPinchGesture(pinchIn(1.0f, 0.0f, 400.0f, 300.0f,
                                        CameraSystem::PinchMode::Undecided,
                                        t));
    trace_.record(*camera_, controller_->groundState());
    for (int i = 1; i <= 10; ++i) {
        t += 0.016;
        controller_->onPinchGesture(
            pinchIn(1.0f + 0.25f * i, 0.0f, 400.0f + 2.0f * i, 300.0f,
                    CameraSystem::PinchMode::Manipulate, t));
        trace_.record(*camera_, controller_->groundState());
    }
    controller_->onPinchEnd();
    trace_.record(*camera_, controller_->groundState());
    step(10);

    EXPECT_GE(altitudeMeters(),
              controller_->groundState().terrainHeightMeters +
                  CameraSystem::kMinClearanceMeters - 1.0)
        << "场景失效:压不到净空面上,钳位分支没走到";

    // 必须先让惯性彻底停下:③ 要验的是**数据驱动**的下降滤波,而位姿只要还在
    // 动,帧末哨兵的指纹比对就会判成 user-driven(滤波恒立即),指数分支走不到。
    int settleGuard = 0;
    while (controller_->isSelfAnimating() && settleGuard++ < 600) {
        step(1);
    }
    ASSERT_FALSE(controller_->isSelfAnimating())
        << "场景失效:惯性未停,③ 会被判成 user-driven,指数分支走不到";

    // ③ 数据驱动的地形骤降:滤波应走 τ 指数逼近(不立即)。
    plateauHeight = 120.0;
    revision = 3;
    step(2);
    const double afterTwoFrames = controller_->groundState().terrainHeightMeters;
    EXPECT_GT(afterTwoFrames, 200.0)
        << "场景失效:骤降被立即吸收,没走到指数逼近分支";
    // τ=0.5s ⇒ 总进度 1−exp(−t/τ)。780→120 要降到 200 以下需走完 87.9%,
    // 即 t≈1.05s(63 帧);给到 2s 留余量。
    step(120);
    EXPECT_LT(controller_->groundState().terrainHeightMeters, 200.0)
        << "场景失效:指数逼近没有收敛";

    EXPECT_GT(sampleCalls, 0);
    EXPECT_TRACE_HASH(trace_, 0xecd490268a7f328cull);
}

// ---------------------------------------------------------------------------
// 场景 D:高空——快速路径(跳过地形采样) + zoom-out 锚点缩放 +
//         帧末哨兵收编绕过控制器的裸写。
// 覆盖:constrainEyeAgainstTerrain 高空早退 / dolly 沿 eye→anchor /
//       resolveConstraints 的位姿指纹分支。(契约 2.4:无高空回中。)
// ---------------------------------------------------------------------------
TEST_F(PoseTraceTest, TraceD_HighAltitudeRecenterAndSentinel) {
    int sampleCalls = 0;
    controller_->setTerrainAreaSampleFunc(makeAnalyticAreaSampler(
        [](double, double) { return 100.0; }, &sampleCalls));

    const auto& e = Ellipsoid::WGS84();
    const Vec3 target = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.5, 29.6, 0.0));
    controller_->viewDistance(target, 9.0e6);
    step(2);
    EXPECT_FALSE(controller_->groundState().hasTerrainData)
        << "场景失效:高空仍在采样,快速路径没走到";

    // 偏心 zoom-out:锚点缩放(契约 2.4:无回中欠账)。
    double t = 0.0;
    controller_->onPinchGesture(pinchIn(1.0f, 0.0f, 560.0f, 200.0f,
                                        CameraSystem::PinchMode::Undecided,
                                        t));
    trace_.record(*camera_, controller_->groundState());
    for (int i = 1; i <= 8; ++i) {
        t += 0.016;
        controller_->onPinchGesture(
            pinchIn(1.0f - 0.06f * i, 0.0f, 560.0f, 200.0f,
                    CameraSystem::PinchMode::Manipulate, t));
        trace_.record(*camera_, controller_->groundState());
    }
    controller_->onPinchEnd();
    trace_.record(*camera_, controller_->groundState());
    step(25);  // 松手后 zoom 惯性/约束沉降

    // 外部裸写(模拟 Facade/JNI 绕过控制器):帧末哨兵应按 user-driven 收编。
    const glm::dvec3 eye = camera_->position().raw();
    camera_->setView(Vec3(eye * 1.0001), camera_->direction(), camera_->up());
    step(4);

    // 2026-08-19:高空球心回中(契约 2.4)——拉远越过 1.5R 后视轴随高度转向地心,
    // TraceD 的偏心 zoom-out 轨迹随之变化,hash 更新。
    EXPECT_TRACE_HASH(trace_, 0x1db2e1a1821446d3ull);
}

// ---------------------------------------------------------------------------
// 场景 E:病态区——球缘掠射 / 拖到球外 / 再扫回球面。
// 覆盖:pointOnGrabSphere 最近接近点分支 / anchorExactWeight 混合带 /
//       turntableDeltaFromPixels 转台回退 / 退化区整点重取锚点。
// ---------------------------------------------------------------------------
TEST_F(PoseTraceTest, TraceE_LimbAndOffGlobeDrag) {
    const auto& e = Ellipsoid::WGS84();
    const Vec3 target = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.5, 29.6, 0.0));
    controller_->viewDistance(target, 2.0e7);  // 全球在视野内,球缘可达
    step(2);

    // 从画面中心一路拖过球缘到画面角落(球外),再扫回来。
    controller_->onDragStart(400.0f, 300.0f, 0.0);
    trace_.record(*camera_, controller_->groundState());
    double t = 0.0;
    for (int i = 1; i <= 14; ++i) {
        t += 0.016;
        controller_->onDragMove(400.0f + 26.0f * i, 300.0f - 18.0f * i, t);
        trace_.record(*camera_, controller_->groundState());
    }
    for (int i = 13; i >= 0; --i) {
        t += 0.016;
        controller_->onDragMove(400.0f + 26.0f * i, 300.0f - 18.0f * i, t);
        trace_.record(*camera_, controller_->groundState());
    }
    controller_->onDragEnd();
    trace_.record(*camera_, controller_->groundState());
    step(10);

    // 2026-08-19:spin 路径采样率改为手指视角角速度(与 anchor 路径同语义);
    // 且退化带转台按"锚点距/地心距"缩放到锚点尺度(防低空绕地心甩出=跳远),
    // 球缘拖拽轨迹随之变化,hash 更新。
    EXPECT_TRACE_HASH(trace_, 0x1b0609ea643220cull);
}

}  // namespace
