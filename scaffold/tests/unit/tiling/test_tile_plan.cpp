#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <unordered_set>
#include "earth_engine/tiling/TilePlan.h"
#include "earth_engine/tiling/TileQuadTree.h"
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

    int maxVisibleZoom = 0;
    for (const auto& key : plan.visibleTiles) {
        EXPECT_EQ("XYZ-WebMercator", key.schemeId);
        EXPECT_GE(key.z, scheme_->minZoom());
        EXPECT_LE(key.z, scheme_->maxZoom());
        maxVisibleZoom = std::max(maxVisibleZoom, key.z);
    }
    EXPECT_EQ(maxVisibleZoom, plan.zoom);
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

TEST_F(TilePlanTest, ParentKeyFollowsExactChildLineage) {
    camera_->lookAt(Vec3(0, 0, 7000000), Vec3::zero(), Vec3::unitY());

    TilePlan plan = TilePlanBuilder::compute(*camera_, *scheme_, 800, 600);

    if (plan.zoom > scheme_->minZoom()) {
        ASSERT_GT(plan.visibleTiles.size(), 0u);
        const TileKey child = plan.visibleTiles.front();
        const TileKey parent = TilePlanBuilder::parentKey(child);
        EXPECT_EQ(child.schemeId, parent.schemeId);
        EXPECT_EQ(child.z - 1, parent.z);
        EXPECT_EQ(child.x / 2, parent.x);
        EXPECT_EQ(child.y / 2, parent.y);
    }
}

TEST(TilePlanOpenGlobusEarthTest, ParentKeyPreservesGroupedYLineage) {
    TileKey northChild{"OpenGlobus-Earth", 4, 7, (1 << 4) + 9};
    TileKey northParent = TilePlanBuilder::parentKey(northChild);
    EXPECT_EQ("OpenGlobus-Earth", northParent.schemeId);
    EXPECT_EQ(3, northParent.z);
    EXPECT_EQ(3, northParent.x);
    EXPECT_EQ((1 << 3) + 4, northParent.y);

    TileKey southChild{"OpenGlobus-Earth", 4, 7, 2 * (1 << 4) + 9};
    TileKey southParent = TilePlanBuilder::parentKey(southChild);
    EXPECT_EQ("OpenGlobus-Earth", southParent.schemeId);
    EXPECT_EQ(3, southParent.z);
    EXPECT_EQ(3, southParent.x);
    EXPECT_EQ(2 * (1 << 3) + 4, southParent.y);
}

TEST_F(TilePlanTest, AllTilesWithinSchemeBounds) {
    camera_->lookAt(Vec3(0, 0, 7000000), Vec3::zero(), Vec3::unitY());

    TilePlan plan = TilePlanBuilder::compute(*camera_, *scheme_, 800, 600);

    for (const auto& key : plan.visibleTiles) {
        int tilesAtZoom = 1 << key.z;
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

    for (const TileKey& key : plan.visibleTiles) {
        int tilesAtZoom = 1 << key.z;
        EXPECT_GE(key.x, 0);
        EXPECT_LT(key.x, tilesAtZoom);
        EXPECT_GE(key.y, 0);
        EXPECT_LT(key.y, tilesAtZoom);
    }
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

    bool sawWestEdge = false;
    bool sawEastEdge = false;
    for (const auto& key : plan.visibleTiles) {
        int tilesAtZoom = 1 << key.z;
        sawWestEdge = sawWestEdge || key.x <= 1;
        sawEastEdge = sawEastEdge || key.x >= tilesAtZoom - 2;
    }
    EXPECT_TRUE(sawWestEdge);
    EXPECT_TRUE(sawEastEdge);
}

TEST_F(TilePlanTest, IncludesSubCameraTileForObliqueEarthView) {
    camera_->setPerspective(glm::radians(60.0), 10000.0, 50000000.0);
    camera_->lookAt(Vec3(7000000, 0, 7000000),
                    Vec3::zero(),
                    Vec3::unitY());

    TilePlan plan = TilePlanBuilder::compute(*camera_, *scheme_, 1240, 2772);
    ASSERT_GT(plan.visibleTiles.size(), 0u);

    TileKey subCamera = scheme_->positionToTile(
        105.0 * M_PI / 180.0,
        35.1808 * M_PI / 180.0,
        plan.zoom);

    bool found = false;
    for (const TileKey& key : plan.visibleTiles) {
        if (key.schemeId != subCamera.schemeId || key.z > subCamera.z) {
            continue;
        }
        const int shift = subCamera.z - key.z;
        found = found ||
            (key.x == (subCamera.x >> shift) && key.y == (subCamera.y >> shift));
    }
    EXPECT_TRUE(found)
        << "sub-camera tile " << subCamera.z << "/"
        << subCamera.x << "/" << subCamera.y
        << " must stay in the frame plan";
}

TEST_F(TilePlanTest, TileQuadTreePersistsNodesAcrossFrames) {
    camera_->lookAt(Vec3(0, 0, 7000000), Vec3::zero(), Vec3::unitY());

    TileQuadTree tree;
    TilePlan first = tree.compute(*camera_, *scheme_, 800, 600);
    ASSERT_GT(first.visibleTiles.size(), 0u);
    ASSERT_NE(nullptr, tree.root());
    EXPECT_TRUE(tree.root()->childrenCreated());
    const int firstNodeCount = tree.createdNodeCount();

    TilePlan second = tree.compute(*camera_, *scheme_, 800, 600, first.zoom);
    EXPECT_EQ(first.zoom, second.zoom);
    EXPECT_EQ(first.visibleTiles, second.visibleTiles);
    EXPECT_EQ(firstNodeCount, tree.createdNodeCount());
}

TEST_F(TilePlanTest, TileQuadTreeNodesFollowParentChildLineage) {
    camera_->lookAt(Vec3(0, 0, 7000000), Vec3::zero(), Vec3::unitY());

    TileQuadTree tree;
    TilePlan plan = tree.compute(*camera_, *scheme_, 800, 600);
    ASSERT_GT(plan.zoom, 0);
    ASSERT_NE(nullptr, tree.root());
    ASSERT_TRUE(tree.root()->childrenCreated());

    const auto& child = tree.root()->children()[0];
    ASSERT_NE(nullptr, child);
    EXPECT_EQ(tree.root(), child->parent());
    EXPECT_EQ(1, child->key().z);
    EXPECT_EQ(0, child->key().x);
    EXPECT_EQ(0, child->key().y);
}

TEST_F(TilePlanTest, TilePlanReportsLodDiagnostics) {
    camera_->lookAt(Vec3(7000000, 0, 0), Vec3::zero(), Vec3::unitZ());

    TilePlan plan = TilePlanBuilder::compute(*camera_, *scheme_, 800, 600);

    EXPECT_GT(plan.renderingNodeCount, 0);
    EXPECT_GE(plan.walkthroughNodeCount, 1);
    EXPECT_GE(plan.notRenderingNodeCount, 0);
    EXPECT_GE(plan.cameraInsideNodeCount, 1);
    EXPECT_GE(plan.minVisibleZoom, 0);
    EXPECT_EQ(plan.zoom, plan.maxVisibleZoom);
    EXPECT_GE(plan.lodSizePixels, plan.maxLodSizePixels);
    EXPECT_LE(plan.lodSizePixels, plan.minLodSizePixels);
    EXPECT_EQ(static_cast<int>(plan.visibleTiles.size()), plan.mercatorTileCount);
    EXPECT_EQ(0, plan.northPolarTileCount);
    EXPECT_EQ(0, plan.southPolarTileCount);
}

TEST_F(TilePlanTest, OpenGlobusEqualZoomPassUsesMaxVisibleZoomForNadirView) {
    camera_->lookAt(Vec3(7000000, 0, 0), Vec3::zero(), Vec3::unitZ());

    TileQuadTree firstPassTree;
    TilePlan firstPass = firstPassTree.compute(*camera_, *scheme_, 800, 600);
    const int firstPassMaxZoom = firstPass.maxVisibleZoom;

    TilePlan plan = TilePlanBuilder::compute(*camera_, *scheme_, 800, 600);

    ASSERT_GT(plan.visibleTiles.size(), 0u);
    EXPECT_TRUE(plan.equalZoomApplied);
    EXPECT_EQ(firstPassMaxZoom, plan.maxVisibleZoom);
    EXPECT_GT(plan.horizonTangentPreservedCount, 0);
    EXPECT_LT(plan.minVisibleZoom, plan.maxVisibleZoom);
    EXPECT_EQ(plan.zoom, plan.maxVisibleZoom);
    EXPECT_GT(plan.neighborLinkCount, 0);
}

TEST_F(TilePlanTest, PersistentTreeReportsOpenGlobusTransitionDiagnostics) {
    TileQuadTree tree;
    camera_->lookAt(Vec3(0, 0, 50000000), Vec3::zero(), Vec3::unitY());
    TilePlan farPlan = tree.compute(*camera_, *scheme_, 800, 600);
    ASSERT_GT(farPlan.visibleTiles.size(), 0u);

    camera_->lookAt(Vec3(0, 0, 6778137), Vec3::zero(), Vec3::unitY());
    TilePlan nearPlan = tree.compute(*camera_, *scheme_, 800, 600);

    EXPECT_GT(nearPlan.visibleTiles.size(), 0u);
    EXPECT_GT(nearPlan.fadingNodeCount, 0);
    EXPECT_GT(nearPlan.neighborLinkCount, 0);
}

TEST_F(TilePlanTest, OpenGlobusEqualZoomPassSkippedOutsideAltitudeBand) {
    camera_->lookAt(Vec3(0, 0, 50000000), Vec3::zero(), Vec3::unitY());

    TilePlan plan = TilePlanBuilder::compute(*camera_, *scheme_, 800, 600);

    EXPECT_FALSE(plan.equalZoomApplied);
    EXPECT_EQ(plan.zoom, plan.maxVisibleZoom);
}

TEST_F(TilePlanTest, NearGroundCameraInsideBranchReachesHeightZoom) {
    // OpenGlobus does NOT force-subdivide in the primary traverse based on
    // camera-inside — the LOD formula alone governs when to stop.  At 1 km
    // altitude the projected size naturally selects zoom ≈ 17.
    constexpr double radius = 6378137.0;
    camera_->setPerspective(glm::radians(60.0), 1.0, 50000000.0);
    camera_->lookAt(Vec3(radius + 1000.0, 0.0, 0.0),
                    Vec3(radius, 0.0, 0.0),
                    Vec3::unitZ());

    TileQuadTree tree;
    TilePlan plan = tree.compute(*camera_, *scheme_, 1240, 2772);

    EXPECT_GE(plan.maxVisibleZoom, 16);
}

TEST(TilePlanOpenGlobusEarthTest, ReportsPolarTileGroups) {
    auto scheme = TileScheme::createOpenGlobusEarth();
    Camera camera;
    camera.setPerspective(glm::radians(60.0), 1.0, 50000000.0);
    camera.lookAt(Vec3(0, 0, 7000000), Vec3::zero(), Vec3::unitY());

    TilePlan plan = TilePlanBuilder::compute(camera, *scheme, 800, 600);

    EXPECT_GT(plan.visibleTiles.size(), 0u);
    EXPECT_EQ(static_cast<int>(plan.visibleTiles.size()),
              plan.mercatorTileCount + plan.northPolarTileCount +
              plan.southPolarTileCount);
}

TEST(TilePlanOpenGlobusEarthTest, PolarViewTraversesPolarRoot) {
    auto scheme = TileScheme::createOpenGlobusEarth();
    Camera camera;
    camera.setPerspective(glm::radians(60.0), 1.0, 50000000.0);
    camera.lookAt(Vec3(0, 0, 7000000), Vec3::zero(), Vec3::unitY());

    TileQuadTree tree;
    TilePlan plan = tree.compute(camera, *scheme, 800, 600);

    ASSERT_GT(plan.visibleTiles.size(), 0u);
    EXPECT_GT(plan.northPolarTileCount, 0);
    for (const TileKey& key : plan.visibleTiles) {
        const int tilesAtZoom = 1 << key.z;
        if (key.y >= tilesAtZoom && key.y < 2 * tilesAtZoom) {
            Rectangle bounds = scheme->tileToRectangle(key);
            EXPECT_GT(bounds.north(), 85.0 * M_PI / 180.0);
        }
    }
}
