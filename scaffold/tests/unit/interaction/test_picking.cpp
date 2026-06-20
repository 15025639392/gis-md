#include <gtest/gtest.h>
#include <cmath>
#include "earth_engine/interaction/PickingService.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/math/Ray.h"

using namespace earth_engine;

class PickingServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        camera_ = std::make_unique<Camera>();
        camera_->setPerspective(glm::radians(60.0), 1.0, 50000000.0);
        // 从上方看向原点
        camera_->lookAt(Vec3(0, 0, 10000000), Vec3::zero(), Vec3::unitY());
        service_ = std::make_unique<PickingService>();
    }

    std::unique_ptr<Camera> camera_;
    std::unique_ptr<PickingService> service_;
};

TEST_F(PickingServiceTest, CenterPixelHitsEllipsoid) {
    // 屏幕中心点应对准地球
    auto result = service_->pickEllipsoid(400, 300, *camera_, 800, 600);

    EXPECT_EQ(PickResult::HitType::Ellipsoid, result.hitType);
    EXPECT_TRUE(result.isValid());
    EXPECT_GT(result.distance, 0.0);
}

TEST_F(PickingServiceTest, CornerPixelMisses) {
    // 从足够远的位置看向地球时，视口角落位于地球视盘外。
    camera_->lookAt(Vec3(0, 0, 20000000), Vec3::zero(), Vec3::unitY());
    auto result = service_->pickEllipsoid(0, 0, *camera_, 800, 600);

    EXPECT_EQ(PickResult::HitType::None, result.hitType);
    EXPECT_FALSE(result.isValid());
    EXPECT_FLOAT_EQ(0.0f, result.screenX);
    EXPECT_FLOAT_EQ(0.0f, result.screenY);
    EXPECT_DOUBLE_EQ(0.0, result.distance);
}

TEST_F(PickingServiceTest, EllipsoidHitPositionIsOnSurface) {
    auto result = service_->pickEllipsoid(400, 300, *camera_, 800, 600);
    ASSERT_TRUE(result.isValid());

    // 命中的点应在椭球面附近
    const auto& e = Ellipsoid::WGS84();
    auto back = e.cartesianToCartographic(result.worldPosition);

    // 高度应接近 0（在椭球表面上）
    EXPECT_NEAR(0.0, back.height(), 10.0);  // 10m 容差
}

TEST_F(PickingServiceTest, RayEllipsoidFromAbove) {
    // 相机在北极上方 → 拾取射线应命中北极附近
    camera_->lookAt(Vec3(0, 0, 7000000), Vec3::zero(), Vec3::unitX());
    auto result = service_->pickEllipsoid(400, 300, *camera_, 800, 600);
    ASSERT_TRUE(result.isValid());

    // 纬度应较高
    EXPECT_GT(result.cartographic.latitude(), 0.5);  // > ~28°
}

TEST_F(PickingServiceTest, PickReturnsCartographic) {
    auto result = service_->pickEllipsoid(400, 300, *camera_, 800, 600);
    ASSERT_TRUE(result.isValid());

    // 经纬度在有效范围
    EXPECT_GE(result.cartographic.longitude(), -M_PI);
    EXPECT_LE(result.cartographic.longitude(), M_PI);
    EXPECT_GE(result.cartographic.latitude(), -M_PI_2);
    EXPECT_LE(result.cartographic.latitude(), M_PI_2);
}

TEST_F(PickingServiceTest, RayTriangleMatchesNativeBackFaceAndOriginHits) {
    const Vec3 v0(-1.0, 0.0, 0.0);
    const Vec3 v1(1.0, 0.0, 0.0);
    const Vec3 v2(0.0, 1.0, 0.0);

    double t = -1.0;
    EXPECT_TRUE(PickingService::rayTriangleIntersection(
        Vec3(0.0, 0.0, -1.0),
        Vec3(0.0, 0.0, 1.0),
        v0,
        v1,
        v2,
        t));
    EXPECT_NEAR(1.0, t, 1e-12);

    t = -1.0;
    EXPECT_TRUE(PickingService::rayTriangleIntersection(
        Vec3(0.0, 0.0, 0.0),
        Vec3(0.0, 0.0, 1.0),
        v0,
        v1,
        v2,
        t));
    EXPECT_NEAR(0.0, t, 1e-12);
}
