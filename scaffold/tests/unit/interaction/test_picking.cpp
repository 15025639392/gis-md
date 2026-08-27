#include <gtest/gtest.h>
#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include "earth_engine/interaction/PickingService.h"
#include "earth_engine/layers/FeatureRenderLayer.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/scene/Scene.h"
#include "earth_engine/scene/ScenePickingCoordinator.h"
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

TEST_F(PickingServiceTest, PickTerrainTreatsZeroHeightAsTerrainHit) {
    auto result = service_->pickTerrain(
        400,
        300,
        *camera_,
        800,
        600,
        [](double, double) {
            return 0.0f;
        });

    EXPECT_EQ(PickResult::HitType::Terrain, result.hitType);
    EXPECT_TRUE(result.isValid());
    EXPECT_FLOAT_EQ(0.0f, result.terrainHeight);
}

namespace {

// 低空相机:置于 (lon0, lat0) 上空 alt 米,朝 east 下俯 lookDownDeg。
Camera makeLowCamera(double lon0Deg, double lat0Deg, double altMeters,
                     double lookDownDeg) {
    Camera cam;
    cam.setPerspective(glm::radians(60.0), 1.0, 5.0e7);
    const auto& e = Ellipsoid::WGS84();
    const Vec3 ground = e.cartographicToCartesian(
        Cartographic::fromDegrees(lon0Deg, lat0Deg, 0.0));
    const glm::dvec3 up = e.geodeticSurfaceNormal(ground).raw();
    const glm::dvec3 east =
        glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up));
    const glm::dvec3 eye = ground.raw() + up * altMeters;
    const double pr = glm::radians(lookDownDeg);
    const glm::dvec3 dir =
        glm::normalize(east * std::cos(pr) - up * std::sin(pr));
    const glm::dvec3 camUp =
        glm::normalize(glm::cross(dir, glm::cross(up, dir)));
    cam.setView(Vec3(eye), Vec3(dir), Vec3(camUp));
    return cam;
}

}  // namespace

// 低空朝悬崖拖拽:拾取必须返回"用户看到的那面"上的点(射线上、相机下方),
// 而不是旧实现"椭球交点+抬升"的山后/高于相机点(锚点贴眼、增益崩塌的根源)。
TEST_F(PickingServiceTest, PickTerrainCliffReturnsNearVisibleFaceOnRay) {
    constexpr double kLon0 = 106.5;  // 崖线经度:东侧 1500m、西侧 0m
    Camera cam = makeLowCamera(kLon0 - 0.0005, 29.6, 1000.0, 30.0);

    PickingService svc;
    const PickResult r = svc.pickTerrain(
        400, 300, cam, 800, 600,
        [](double lng, double lat) -> float {
            (void)lat;
            return lng >= glm::radians(kLon0) ? 1500.0f : 0.0f;
        });

    ASSERT_EQ(PickResult::HitType::Terrain, r.hitType);
    ASSERT_TRUE(r.isValid());
    // 崖面在相机前方 ~50m(经度差 0.0005°≈48m/水平投影 cos30°);旧实现返回
    // 椭球入口(≈2000m 远、抬升后高于相机)。
    EXPECT_GT(r.distance, 20.0);
    EXPECT_LT(r.distance, 300.0);
    EXPECT_FLOAT_EQ(1500.0f, r.terrainHeight);

    // 命中点在拾取射线上(旧实现返回点在射线外,控制器靠重投影兜底)。
    const Ray ray = cam.getPickRay(400.0, 300.0, 800.0, 600.0);
    EXPECT_NEAR(0.0,
                glm::length(r.worldPosition.raw() -
                            ray.pointAt(r.distance).raw()),
                1e-6);
    // 命中点必须在相机下方(旧实现抬升点 1500m > 相机 1000m,触发半径钳制)。
    const double hitH =
        Ellipsoid::WGS84()
            .cartesianToCartographic(r.worldPosition)
            .height();
    EXPECT_LT(hitH, 1000.0);
    EXPECT_GT(hitH, 500.0);  // 在崖面上,不是椭球面 0m
}

// 高原(500m)上空低俯视:交点应在射线高度=500m 处(≈1000m 距离),验证射线
// 中途交而非崖沿特例。
TEST_F(PickingServiceTest, PickTerrainPlateauCrossingAtMidRay) {
    Camera cam = makeLowCamera(106.5, 29.6, 1000.0, 30.0);

    PickingService svc;
    const PickResult r = svc.pickTerrain(
        400, 300, cam, 800, 600,
        [](double, double) -> float { return 500.0f; });

    ASSERT_EQ(PickResult::HitType::Terrain, r.hitType);
    // 1000m 高、30° 下俯:射线高度 1000 − 0.5·t 在 t=1000 处降到 500m。
    EXPECT_NEAR(1000.0, r.distance, 30.0);
    EXPECT_FLOAT_EQ(500.0f, r.terrainHeight);
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

TEST(ScenePickingCoordinatorTest,
     PicksFeatureRenderLayerAndPreservesStableIdentity) {
    Camera camera;
    camera.setPerspective(glm::radians(60.0), 1.0, 50000000.0);
    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, 0.0));
    camera.lookAt(surface + Vec3(40000.0, 0.0, 0.0), surface, Vec3::unitZ());

    FrameState frame;
    frame.camera = &camera;
    frame.viewportWidthPixels = 800;
    frame.viewportHeightPixels = 600;

    auto layer = std::make_unique<FeatureRenderLayer>(
        "scene-feature", nullptr, Ellipsoid::WGS84());
    Feature feature;
    feature.type = GeometryType::Polygon;
    feature.sourceId = "source-42";
    feature.rings = {{
        Cartographic(-0.05 * M_PI / 180.0, -0.05 * M_PI / 180.0),
        Cartographic(0.05 * M_PI / 180.0, -0.05 * M_PI / 180.0),
        Cartographic(0.05 * M_PI / 180.0, 0.05 * M_PI / 180.0),
        Cartographic(-0.05 * M_PI / 180.0, 0.05 * M_PI / 180.0),
        Cartographic(-0.05 * M_PI / 180.0, -0.05 * M_PI / 180.0)}};
    const FeatureId featureId = layer->store().addFeature(std::move(feature));

    std::vector<std::unique_ptr<FeatureRenderLayer>> layers;
    layers.push_back(std::move(layer));
    PickingService service;
    ScenePickingContext context;
    context.pickingService = &service;
    context.camera = &camera;
    context.viewportWidthPixels = 800.0;
    context.viewportHeightPixels = 600.0;
    context.frameState = &frame;
    context.featureRenderLayers = &layers;

    const PickResult result =
        ScenePickingCoordinator::pick(context, 400.0f, 300.0f);

    ASSERT_EQ(PickResult::HitType::VectorFeature, result.hitType);
    EXPECT_EQ(PickResult::FeatureSourceKind::FeatureRenderLayer,
              result.sourceKind);
    EXPECT_EQ(PickResult::FeaturePart::Fill, result.featurePart);
    EXPECT_EQ("scene-feature", result.layerId);
    EXPECT_EQ(std::to_string(featureId), result.featureId);
    EXPECT_EQ(featureId, result.featureNumericId);
    EXPECT_EQ("source-42", result.sourceFeatureId);
    EXPECT_GE(result.distance, 0.0);
    EXPECT_GT(result.worldPosition.length(), 6.0e6);
}

TEST(ScenePickingCoordinatorTest,
     AbsoluteFillUsesRenderedFeatureHeightForDepth) {
    Camera camera;
    camera.setPerspective(glm::radians(60.0), 1.0, 50000000.0);
    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, 0.0));
    camera.lookAt(surface + Vec3(40000.0, 0.0, 0.0), surface, Vec3::unitZ());

    FrameState frame;
    frame.camera = &camera;
    frame.viewportWidthPixels = 800;
    frame.viewportHeightPixels = 600;

    auto layer = std::make_unique<FeatureRenderLayer>(
        "absolute-fill", nullptr, Ellipsoid::WGS84());
    Feature feature;
    feature.type = GeometryType::Polygon;
    feature.rings = {{
        Cartographic(-0.05 * M_PI / 180.0,
                     -0.05 * M_PI / 180.0, 800.0),
        Cartographic(0.05 * M_PI / 180.0,
                     -0.05 * M_PI / 180.0, 800.0),
        Cartographic(0.05 * M_PI / 180.0,
                     0.05 * M_PI / 180.0, 800.0),
        Cartographic(-0.05 * M_PI / 180.0,
                     0.05 * M_PI / 180.0, 800.0),
        Cartographic(-0.05 * M_PI / 180.0,
                     -0.05 * M_PI / 180.0, 800.0)}};
    const FeatureId featureId = layer->store().addFeature(std::move(feature));

    std::vector<std::unique_ptr<FeatureRenderLayer>> layers;
    layers.push_back(std::move(layer));
    PickingService service;
    ScenePickingContext context;
    context.pickingService = &service;
    context.camera = &camera;
    context.viewportWidthPixels = 800.0;
    context.viewportHeightPixels = 600.0;
    context.frameState = &frame;
    context.featureRenderLayers = &layers;

    const PickResult result =
        ScenePickingCoordinator::pick(context, 400.0f, 300.0f);

    ASSERT_EQ(PickResult::HitType::VectorFeature, result.hitType);
    EXPECT_EQ(PickResult::FeatureSourceKind::FeatureRenderLayer,
              result.sourceKind);
    EXPECT_EQ(featureId, result.featureNumericId);
    EXPECT_EQ(PickResult::FeaturePart::Fill, result.featurePart);
    // Default fill mesh is a planar ECEF triangle fan, so this 0.1° square's
    // center is ~4.8 m below the ideal 800 m ellipsoid surface. The result
    // must match that rendered mesh and, critically, must not flatten to 0 m.
    EXPECT_NEAR(800.0, result.cartographic.height(), 10.0);
}

TEST(ScenePickingCoordinatorTest, HiddenFeatureRenderLayerIsNotPickable) {
    Camera camera;
    camera.setPerspective(glm::radians(60.0), 1.0, 50000000.0);
    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, 0.0));
    camera.lookAt(surface + Vec3(40000.0, 0.0, 0.0), surface, Vec3::unitZ());

    FrameState frame;
    frame.camera = &camera;
    frame.viewportWidthPixels = 800;
    frame.viewportHeightPixels = 600;

    auto layer = std::make_unique<FeatureRenderLayer>(
        "hidden-feature", nullptr, Ellipsoid::WGS84());
    Feature feature;
    feature.type = GeometryType::Point;
    feature.rings = {{Cartographic(0.0, 0.0)}};
    layer->store().addFeature(std::move(feature));
    layer->setVisible(false);

    std::vector<std::unique_ptr<FeatureRenderLayer>> layers;
    layers.push_back(std::move(layer));
    PickingService service;
    ScenePickingContext context;
    context.pickingService = &service;
    context.camera = &camera;
    context.viewportWidthPixels = 800.0;
    context.viewportHeightPixels = 600.0;
    context.frameState = &frame;
    context.featureRenderLayers = &layers;

    const PickResult result =
        ScenePickingCoordinator::pick(context, 400.0f, 300.0f);

    EXPECT_EQ(PickResult::HitType::Ellipsoid, result.hitType);
    EXPECT_EQ(PickResult::FeatureSourceKind::None, result.sourceKind);
}

TEST(ScenePickingCoordinatorTest, ScenePickUsesFeatureRenderLayerRegistry) {
    Scene scene;
    scene.setViewport(800, 600);
    scene.camera().setPerspective(glm::radians(60.0), 1.0, 50000000.0);
    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, 0.0));
    scene.camera().lookAt(
        surface + Vec3(40000.0, 0.0, 0.0), surface, Vec3::unitZ());

    auto layer = std::make_unique<FeatureRenderLayer>(
        "scene-registry", nullptr, Ellipsoid::WGS84());
    Feature feature;
    feature.type = GeometryType::Point;
    feature.rings = {{Cartographic(0.0, 0.0)}};
    const FeatureId featureId = layer->store().addFeature(std::move(feature));
    scene.addFeatureRenderLayer(std::move(layer));

    const PickResult result = scene.pick(400.0f, 300.0f);

    ASSERT_EQ(PickResult::HitType::VectorFeature, result.hitType);
    EXPECT_EQ(PickResult::FeatureSourceKind::FeatureRenderLayer,
              result.sourceKind);
    EXPECT_EQ("scene-registry", result.layerId);
    EXPECT_EQ(std::to_string(featureId), result.featureId);
    EXPECT_EQ(PickResult::FeaturePart::Vertex, result.featurePart);
}

TEST(ScenePickingCoordinatorTest, FeatureBehindSurfaceDoesNotBeatFallback) {
    Camera camera;
    camera.setPerspective(glm::radians(60.0), 1.0, 50000000.0);
    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, 0.0));
    camera.lookAt(surface + Vec3(40000.0, 0.0, 0.0), surface, Vec3::unitZ());

    FrameState frame;
    frame.camera = &camera;
    frame.viewportWidthPixels = 800;
    frame.viewportHeightPixels = 600;

    auto layer = std::make_unique<FeatureRenderLayer>(
        "buried-feature", nullptr, Ellipsoid::WGS84());
    Feature feature;
    feature.type = GeometryType::Point;
    feature.rings = {{Cartographic(0.0, 0.0, -1000.0)}};
    layer->store().addFeature(std::move(feature));

    std::vector<std::unique_ptr<FeatureRenderLayer>> layers;
    layers.push_back(std::move(layer));
    PickingService service;
    ScenePickingContext context;
    context.pickingService = &service;
    context.camera = &camera;
    context.viewportWidthPixels = 800.0;
    context.viewportHeightPixels = 600.0;
    context.frameState = &frame;
    context.featureRenderLayers = &layers;

    const PickResult result =
        ScenePickingCoordinator::pick(context, 400.0f, 300.0f);

    EXPECT_EQ(PickResult::HitType::Ellipsoid, result.hitType);
    EXPECT_EQ(PickResult::FeatureSourceKind::None, result.sourceKind);
}

TEST(ScenePickingCoordinatorTest, PreservesLineEdgeDetails) {
    Camera camera;
    camera.setPerspective(glm::radians(60.0), 1.0, 50000000.0);
    const Vec3 surface = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic(0.0, 0.0));
    camera.lookAt(surface + Vec3(40000.0, 0.0, 0.0), surface, Vec3::unitZ());

    FrameState frame;
    frame.camera = &camera;
    frame.viewportWidthPixels = 800;
    frame.viewportHeightPixels = 600;

    auto layer = std::make_unique<FeatureRenderLayer>(
        "line-feature", nullptr, Ellipsoid::WGS84());
    Feature feature;
    feature.type = GeometryType::LineString;
    // A one-degree surface segment has a ~243 m ECEF chord sag at its
    // midpoint. Unified picking must use a surface/rendered position rather
    // than classify that chord point as hidden behind the ellipsoid fallback.
    feature.rings = {{Cartographic(-0.5 * M_PI / 180.0, 0.0),
                      Cartographic(0.5 * M_PI / 180.0, 0.0)}};
    const FeatureId featureId = layer->store().addFeature(std::move(feature));

    std::vector<std::unique_ptr<FeatureRenderLayer>> layers;
    layers.push_back(std::move(layer));
    PickingService service;
    ScenePickingContext context;
    context.pickingService = &service;
    context.camera = &camera;
    context.viewportWidthPixels = 800.0;
    context.viewportHeightPixels = 600.0;
    context.frameState = &frame;
    context.featureRenderLayers = &layers;

    const PickResult result =
        ScenePickingCoordinator::pick(context, 400.0f, 300.0f);

    ASSERT_EQ(PickResult::HitType::VectorFeature, result.hitType);
    EXPECT_EQ(PickResult::FeatureSourceKind::FeatureRenderLayer,
              result.sourceKind);
    EXPECT_EQ(PickResult::FeaturePart::Edge, result.featurePart);
    EXPECT_EQ(featureId, result.featureNumericId);
    EXPECT_EQ(0, result.ringIndex);
    EXPECT_EQ(0, result.vertexIndex);
    EXPECT_NEAR(0.0, result.cartographic.height(), 1e-3);
}
