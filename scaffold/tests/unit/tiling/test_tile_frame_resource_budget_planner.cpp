#include <gtest/gtest.h>

#include "earth_engine/tiling/TileFrameResourceBudgetPlanner.h"

using namespace earth_engine;

TEST(TileFrameResourceBudgetPlannerTest, AlignsRasterBudgetWithTransportLane) {
    const FrameResourceBudgetConfig defaultConfig =
        TileFrameResourceBudgetPlanner::plan(
            TileFrameResourceBudgetPlanInput::withTransportLimit(
                20,
                20,
                0.0,
                false,
                false));
    EXPECT_EQ(defaultConfig.maxNetworkRequestsPerFrame, 20u);
    EXPECT_EQ(defaultConfig.maxTerrainContentNetworkRequestsPerFrame, 20u);
    EXPECT_EQ(defaultConfig.maxRasterNetworkRequestsPerFrame, 20u);
    EXPECT_EQ(defaultConfig.maxNetworkInflight, 20u);
    EXPECT_EQ(defaultConfig.maxTerrainContentNetworkInflight, 20u);
    EXPECT_EQ(defaultConfig.maxRasterNetworkInflight, 20u);

    const FrameResourceBudgetConfig tinyConfig =
        TileFrameResourceBudgetPlanner::plan(
            TileFrameResourceBudgetPlanInput::withTransportLimit(
                2,
                20,
                0.0,
                false,
                false));
    FrameResourceBudget budget;
    budget.beginFrame(1, tinyConfig);
    EXPECT_EQ(tinyConfig.maxRasterNetworkRequestsPerFrame, 20u);
    EXPECT_EQ(tinyConfig.maxRasterNetworkInflight, 20u);
    EXPECT_TRUE(budget.tryIssue(
        FrameResourceLane::RasterRequest,
        FrameResourcePriority::Normal,
        20));
    EXPECT_FALSE(budget.tryIssue(
        FrameResourceLane::RasterRequest,
        FrameResourcePriority::Normal,
        1));

    FrameResourceBudget secondBudget;
    secondBudget.beginFrame(2, tinyConfig);
    EXPECT_TRUE(secondBudget.hasNetworkInflightCapacity(
        FrameResourceLane::RasterRequest,
        0,
        20));
    EXPECT_FALSE(secondBudget.hasNetworkInflightCapacity(
        FrameResourceLane::RasterRequest,
        1,
        20));

    const FrameResourceBudgetConfig wideTransportConfig =
        TileFrameResourceBudgetPlanner::plan(
            TileFrameResourceBudgetPlanInput::withTransportLimit(
                2,
                40,
                0.0,
                false,
                false));
    EXPECT_EQ(wideTransportConfig.maxRasterNetworkRequestsPerFrame, 32u);
    EXPECT_EQ(wideTransportConfig.maxRasterNetworkInflight, 32u);
}
