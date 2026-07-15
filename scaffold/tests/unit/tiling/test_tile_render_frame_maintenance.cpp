#include <gtest/gtest.h>

#include "earth_engine/tiling/TileRenderFrameMaintenance.h"

using namespace earth_engine;

TEST(
    TileRenderFrameMaintenanceTest,
    TrimsIncrementalRasterLruWithoutRunningTerrainUnloadUnderBudget) {
    int trimCalls = 0;
    int unloadCalls = 0;

    const TileRenderFrameMaintenanceTimings timings =
        TileRenderFrameMaintenance::run(
            [&](bool cachePressure) {
                EXPECT_FALSE(cachePressure);
                ++trimCalls;
            },
            []() {
                return false;
            },
            [&]() {
                ++unloadCalls;
            });

    EXPECT_EQ(1, trimCalls);
    EXPECT_EQ(0, unloadCalls);
    EXPECT_DOUBLE_EQ(0.0, timings.detachInactiveMs);
    EXPECT_DOUBLE_EQ(0.0, timings.eligibilityMs);
    EXPECT_DOUBLE_EQ(0.0, timings.cacheBytesMs);
}

TEST(
    TileRenderFrameMaintenanceTest,
    RunsBudgetedTerrainUnloadOnlyWhenLiveAccountingRequiresIt) {
    int trimCalls = 0;
    int unloadCalls = 0;

    TileRenderFrameMaintenance::run(
        [&](bool cachePressure) {
            EXPECT_FALSE(cachePressure);
            ++trimCalls;
        },
        []() {
            return true;
        },
        [&]() {
            ++unloadCalls;
        });

    EXPECT_EQ(1, trimCalls);
    EXPECT_EQ(1, unloadCalls);
}
