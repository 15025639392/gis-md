#include <gtest/gtest.h>

#include "earth_engine/core/resources/FrameResourceBudget.h"

using namespace earth_engine;

TEST(FrameResourceBudgetTest, DefaultNetworkLimitAppliesPerLaneNotGlobally) {
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 2;
    config.maxNetworkInflight = 2;

    FrameResourceBudget budget;
    budget.beginFrame(7, config);

    EXPECT_TRUE(budget.tryIssue(
        FrameResourceLane::TerrainRequest,
        FrameResourcePriority::Normal));
    EXPECT_TRUE(budget.tryIssue(
        FrameResourceLane::ContentRequest,
        FrameResourcePriority::Normal));
    EXPECT_FALSE(budget.canIssue(
        FrameResourceLane::TerrainRequest,
        FrameResourcePriority::Normal));

    EXPECT_TRUE(budget.tryIssue(
        FrameResourceLane::RasterRequest,
        FrameResourcePriority::Normal));
    EXPECT_TRUE(budget.tryIssue(
        FrameResourceLane::RasterRequest,
        FrameResourcePriority::Normal));
    EXPECT_FALSE(budget.canIssue(
        FrameResourceLane::RasterRequest,
        FrameResourcePriority::Normal));

    EXPECT_EQ(4u, budget.networkRequestsIssued());
    EXPECT_EQ(2u, budget.terrainContentNetworkRequestsIssued());
    EXPECT_EQ(2u, budget.rasterNetworkRequestsIssued());

    const FrameResourceBudgetSnapshot snapshot = budget.snapshot();
    EXPECT_EQ(2u, snapshot.maxNetworkRequestsPerFrame);
    EXPECT_EQ(2u, snapshot.maxTerrainContentNetworkRequestsPerFrame);
    EXPECT_EQ(2u, snapshot.maxRasterNetworkRequestsPerFrame);
    EXPECT_EQ(2u, snapshot.maxNetworkInflight);
    EXPECT_EQ(2u, snapshot.maxTerrainContentNetworkInflight);
    EXPECT_EQ(2u, snapshot.maxRasterNetworkInflight);
}

TEST(FrameResourceBudgetTest, LaneSpecificLimitsOverrideDefaultNetworkLimit) {
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 2;
    config.maxTerrainContentNetworkRequestsPerFrame = 1;
    config.maxRasterNetworkRequestsPerFrame = 3;
    config.maxNetworkInflight = 2;
    config.maxTerrainContentNetworkInflight = 1;
    config.maxRasterNetworkInflight = 3;

    FrameResourceBudget budget;
    budget.beginFrame(8, config);

    EXPECT_TRUE(budget.tryIssue(
        FrameResourceLane::TerrainRequest,
        FrameResourcePriority::Normal));
    EXPECT_FALSE(budget.canIssue(
        FrameResourceLane::ContentRequest,
        FrameResourcePriority::Normal));

    EXPECT_TRUE(budget.tryIssue(
        FrameResourceLane::RasterRequest,
        FrameResourcePriority::Normal,
        3));
    EXPECT_FALSE(budget.canIssue(
        FrameResourceLane::RasterRequest,
        FrameResourcePriority::Normal));

    EXPECT_FALSE(budget.hasNetworkInflightCapacity(
        FrameResourceLane::TerrainRequest,
        1));
    EXPECT_TRUE(budget.hasNetworkInflightCapacity(
        FrameResourceLane::RasterRequest,
        0,
        3));
    EXPECT_FALSE(budget.hasNetworkInflightCapacity(
        FrameResourceLane::RasterRequest,
        1,
        3));

    const FrameResourceBudgetSnapshot snapshot = budget.snapshot();
    EXPECT_EQ(2u, snapshot.maxNetworkRequestsPerFrame);
    EXPECT_EQ(1u, snapshot.maxTerrainContentNetworkRequestsPerFrame);
    EXPECT_EQ(3u, snapshot.maxRasterNetworkRequestsPerFrame);
    EXPECT_EQ(2u, snapshot.maxNetworkInflight);
    EXPECT_EQ(1u, snapshot.maxTerrainContentNetworkInflight);
    EXPECT_EQ(3u, snapshot.maxRasterNetworkInflight);
}

TEST(FrameResourceBudgetTest, RasterFanoutDoesNotConsumeTerrainContentLane) {
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 1;
    config.maxTerrainContentNetworkRequestsPerFrame = 1;
    config.maxRasterNetworkRequestsPerFrame = 4;
    config.maxNetworkInflight = 2;
    config.maxTerrainContentNetworkInflight = 2;
    config.maxRasterNetworkInflight = 4;

    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    EXPECT_TRUE(budget.tryIssue(
        FrameResourceLane::RasterRequest,
        FrameResourcePriority::Normal,
        4));
    EXPECT_TRUE(budget.tryIssue(
        FrameResourceLane::TerrainRequest,
        FrameResourcePriority::Normal));
    EXPECT_FALSE(budget.canIssue(
        FrameResourceLane::ContentRequest,
        FrameResourcePriority::Normal));
    EXPECT_EQ(4u, budget.rasterNetworkRequestsIssued());
    EXPECT_EQ(1u, budget.terrainContentNetworkRequestsIssued());
    EXPECT_EQ(5u, budget.networkRequestsIssued());
}

TEST(FrameResourceBudgetTest, RasterOverlayMappingHasIndependentCountAndTimeCaps) {
    FrameResourceBudgetConfig config;
    config.maxRasterOverlayMappingsPerFrame = 2;
    config.rasterOverlayMappingTimeMs = 0.75;

    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    EXPECT_TRUE(budget.tryStartRasterOverlayMapping());
    budget.recordRasterOverlayMappingElapsed(0.25);
    EXPECT_TRUE(budget.tryStartRasterOverlayMapping());
    EXPECT_FALSE(budget.tryStartRasterOverlayMapping());

    const FrameResourceBudgetSnapshot countSnapshot = budget.snapshot();
    EXPECT_EQ(2u, countSnapshot.rasterOverlayMappingsUsed);
    EXPECT_EQ(2u, countSnapshot.maxRasterOverlayMappingsPerFrame);
    EXPECT_DOUBLE_EQ(0.25, countSnapshot.rasterOverlayMappingElapsedMs);
    EXPECT_DOUBLE_EQ(0.75, countSnapshot.rasterOverlayMappingTimeMs);

    budget.beginFrame(2, config);
    budget.recordRasterOverlayMappingElapsed(0.75);
    EXPECT_FALSE(budget.tryStartRasterOverlayMapping());
    EXPECT_EQ(0u, budget.rasterOverlayMappingsUsed());
}

// ---- I-P2:Urgent 在预算紧张帧可超额,不被 preload 占满饿死 ----

TEST(FrameResourceBudgetTest, UrgentBreaksThroughFullNormalBudget) {
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 2;
    config.maxNetworkInflight = 2;
    // 显式默认:Urgent 每帧可超额 2 个网络请求。
    config.reservedUrgentNetworkRequestsPerFrame = 2;

    FrameResourceBudget budget;
    budget.beginFrame(21, config);

    // 普通请求占满 limit=2。
    EXPECT_TRUE(budget.tryIssue(
        FrameResourceLane::TerrainRequest,
        FrameResourcePriority::Normal));
    EXPECT_TRUE(budget.tryIssue(
        FrameResourceLane::TerrainRequest,
        FrameResourcePriority::Preload));
    EXPECT_FALSE(budget.canIssue(
        FrameResourceLane::TerrainRequest,
        FrameResourcePriority::Normal));

    // 预算满后 Urgent 仍可突破(limit+reserved=4)。
    EXPECT_TRUE(budget.tryIssue(
        FrameResourceLane::TerrainRequest,
        FrameResourcePriority::Urgent));
    EXPECT_TRUE(budget.tryIssue(
        FrameResourceLane::TerrainRequest,
        FrameResourcePriority::Urgent));
    // 突破上限也被封顶。
    EXPECT_FALSE(budget.canIssue(
        FrameResourceLane::TerrainRequest,
        FrameResourcePriority::Urgent));

    // 非 Urgent 不因 Urgent 的存在而获得突破(普通仍被 limit 卡住)。
    EXPECT_FALSE(budget.canIssue(
        FrameResourceLane::TerrainRequest,
        FrameResourcePriority::Normal));
}

TEST(FrameResourceBudgetTest, SnapshotExposesUrgentReservedSlots) {
    FrameResourceBudgetConfig config;
    config.reservedUrgentNetworkRequestsPerFrame = 3;

    FrameResourceBudget budget;
    budget.beginFrame(22, config);
    const FrameResourceBudgetSnapshot snapshot = budget.snapshot();
    EXPECT_EQ(3u, snapshot.reservedUrgentNetworkRequestsPerFrame);
}
