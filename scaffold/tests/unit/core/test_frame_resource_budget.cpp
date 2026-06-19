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
}
