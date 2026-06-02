#include <gtest/gtest.h>
#include <cmath>
#include <unordered_set>
#include "earth_engine/tiling/TilePlan.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/scene/Camera.h"

using namespace earth_engine;

class TilePlanTest : public ::testing::Test {
protected:
    void SetUp() override {
        scheme_ = TileScheme::createXYZWebMercator();
        camera_ = std::make_unique<Camera>();
        camera_->setPerspective(glm::radians(60.0), 1.0, 50000000.0);
    }

    std::unique_ptr<TileScheme> scheme_;
    std::unique_ptr<Camera> camera_;
};

TEST_F(TilePlanTest, ZoomDecreasesWithHeight) {
    // 低轨道 → 高 zoom
    int lowZoom = TilePlanBuilder::zoomLevelFromHeight(
        400000.0,  // 400 km (ISS height)
        600.0, 60.0 * M_PI / 180.0, 256, 0, 20);

    // 高轨道 → 低 zoom
    int highZoom = TilePlanBuilder::zoomLevelFromHeight(
        36000000.0,  // geostationary
        600.0, 60.0 * M_PI / 180.0, 256, 0, 20);

    EXPECT_GT(lowZoom, highZoom);
}

TEST_F(TilePlanTest, ZoomClampedToRange) {
    int zoom = TilePlanBuilder::zoomLevelFromHeight(
        100.0, 600.0, 60.0 * M_PI / 180.0, 256, 3, 15);
    EXPECT_GE(zoom, 3);
    EXPECT_LE(zoom, 15);
}

TEST_F(TilePlanTest, DefaultCameraShowsTiles) {
    // 默认相机：位置 (0, 0, 7000000)，看向原点
    camera_->lookAt(Vec3(0, 0, 7000000), Vec3::zero(), Vec3::unitY());

    TilePlan plan = TilePlanBuilder::compute(*camera_, *scheme_, 800, 600);

    // 应该有可见的瓦片
    EXPECT_GT(plan.visibleTiles.size(), 0u);

    // zoom 应在合理范围
    EXPECT_GE(plan.zoom, 0);
    EXPECT_LE(plan.zoom, 20);

    // 所有 tile key 应属于正确的 scheme
    for (const auto& key : plan.visibleTiles) {
        EXPECT_EQ("XYZ-WebMercator", key.schemeId);
        EXPECT_EQ(plan.zoom, key.z);
    }
}

TEST_F(TilePlanTest, LowAltitudeMoreTiles) {
    // 低轨道（近）→ 更多高 zoom tile
    camera_->lookAt(Vec3(0, 0, 7000000), Vec3::zero(), Vec3::unitY());
    TilePlan nearPlan = TilePlanBuilder::compute(*camera_, *scheme_, 800, 600);

    // 高轨道（远）→ 更少低 zoom tile
    camera_->lookAt(Vec3(0, 0, 50000000), Vec3::zero(), Vec3::unitY());
    TilePlan farPlan = TilePlanBuilder::compute(*camera_, *scheme_, 800, 600);

    // 远处的 zoom 层级应更低
    EXPECT_GE(nearPlan.zoom, farPlan.zoom);
}

TEST_F(TilePlanTest, ParentTilesExistWhenNotMinZoom) {
    camera_->lookAt(Vec3(0, 0, 7000000), Vec3::zero(), Vec3::unitY());

    TilePlan plan = TilePlanBuilder::compute(*camera_, *scheme_, 800, 600);

    if (plan.zoom > scheme_->minZoom()) {
        EXPECT_GT(plan.parentTiles.size(), 0u);
        for (const auto& key : plan.parentTiles) {
            EXPECT_EQ(plan.zoom - 1, key.z);
        }
    }
}

TEST_F(TilePlanTest, AllTilesWithinSchemeBounds) {
    camera_->lookAt(Vec3(0, 0, 7000000), Vec3::zero(), Vec3::unitY());

    TilePlan plan = TilePlanBuilder::compute(*camera_, *scheme_, 800, 600);

    int tilesAtZoom = 1 << plan.zoom;
    for (const auto& key : plan.visibleTiles) {
        EXPECT_GE(key.x, 0);
        EXPECT_LT(key.x, tilesAtZoom);
        EXPECT_GE(key.y, 0);
        EXPECT_LT(key.y, tilesAtZoom);
    }
}

TEST_F(TilePlanTest, VisibleTilesAreDeduplicatedAndBudgetedByZoomWorldSize) {
    camera_->lookAt(Vec3(0, 0, 50000000), Vec3::zero(), Vec3::unitY());

    TilePlan plan = TilePlanBuilder::compute(*camera_, *scheme_, 800, 600);

    std::unordered_set<TileKey> unique(plan.visibleTiles.begin(),
                                       plan.visibleTiles.end());
    EXPECT_EQ(unique.size(), plan.visibleTiles.size());

    int tilesAtZoom = 1 << plan.zoom;
    EXPECT_LE(plan.visibleTiles.size(),
              static_cast<size_t>(tilesAtZoom * tilesAtZoom));
}

TEST_F(TilePlanTest, AntimeridianViewWrapsWithoutDuplicatingTiles) {
    constexpr double radius = 6378137.0;
    const double lng = glm::radians(179.0);
    const double lat = glm::radians(0.0);
    Vec3 eye(std::cos(lat) * std::cos(lng) * radius * 7.0,
             std::cos(lat) * std::sin(lng) * radius * 7.0,
             std::sin(lat) * radius * 7.0);
    camera_->lookAt(eye, Vec3::zero(), Vec3::unitZ());

    TilePlan plan = TilePlanBuilder::compute(*camera_, *scheme_, 800, 600);
    ASSERT_GT(plan.visibleTiles.size(), 0u);

    std::unordered_set<TileKey> unique(plan.visibleTiles.begin(),
                                       plan.visibleTiles.end());
    EXPECT_EQ(unique.size(), plan.visibleTiles.size());

    int tilesAtZoom = 1 << plan.zoom;
    bool sawWestEdge = false;
    bool sawEastEdge = false;
    for (const auto& key : plan.visibleTiles) {
        sawWestEdge = sawWestEdge || key.x <= 1;
        sawEastEdge = sawEastEdge || key.x >= tilesAtZoom - 2;
    }
    EXPECT_TRUE(sawWestEdge);
    EXPECT_TRUE(sawEastEdge);
}
