#include <gtest/gtest.h>

#include "MinimalGlobeDemoConfig.h"

#include <string>

using earth_engine::minimal_globe_demo::AmapWorkerBudget;
using earth_engine::minimal_globe_demo::chooseAmapWorkerBudget;
using earth_engine::minimal_globe_demo::makeDefaultDemoSceneConfig;

TEST(AmapWorkerBudget, KeepsDecodeIndependentOnLowMemoryPhone) {
    constexpr int64_t kGiB = 1024LL * 1024LL * 1024LL;
    const AmapWorkerBudget budget = chooseAmapWorkerBudget(8, 4 * kGiB);
    EXPECT_EQ(budget.decodeThreads, 1u);
    EXPECT_EQ(budget.poiDecodeThreads, 1u);
    EXPECT_EQ(budget.tessellationThreads, 2u);
}

TEST(AmapWorkerBudget, UsesBoundedParallelismOnHighMemoryPhone) {
    constexpr int64_t kGiB = 1024LL * 1024LL * 1024LL;
    const AmapWorkerBudget budget = chooseAmapWorkerBudget(8, 16 * kGiB);
    EXPECT_EQ(budget.decodeThreads, 2u);
    EXPECT_EQ(budget.poiDecodeThreads, 1u);
    EXPECT_EQ(budget.tessellationThreads, 3u);
    EXPECT_LE(budget.decodeThreads + budget.poiDecodeThreads +
                  budget.tessellationThreads,
              6u);
}

TEST(DemoSourceConfig, PureVectorAmapStyleHasNoRasterOverlays) {
    using namespace earth_engine::minimal_globe_demo;
    const auto config = makeDefaultDemoSceneConfig();
    EXPECT_FALSE(config.terrainPageStore);
    EXPECT_TRUE(config.rasterOverlays.empty());
    EXPECT_EQ(config.terrain.kind, earth_engine::TerrainSourceKind::None);
    EXPECT_FALSE(config.gltf.enabled);
}
