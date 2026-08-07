#include <gtest/gtest.h>

#include "earth_engine/tiling/TilePlan.h"

using namespace earth_engine;

TEST(TileRenderEntryTest, DirectEntryHasSelectedPassOnly) {
    TileRenderEntry entry;
    entry.reason = TileRenderEntryReason::Direct;

    EXPECT_FALSE(entry.isAncestorFallback());
    EXPECT_FALSE(entry.hasSurfaceClip());
    EXPECT_EQ(entry.renderPass(), TileRenderEntryPass::Selected);
}

TEST(TileRenderEntryTest, FallbackEntryExposesClippedAncestorRole) {
    TileRenderEntry entry;
    entry.reason = TileRenderEntryReason::AncestorFallback;
    entry.usesAncestorFallback = true;
    entry.surfaceClipEnabled = true;

    EXPECT_TRUE(entry.isAncestorFallback());
    EXPECT_TRUE(entry.hasSurfaceClip());
    EXPECT_EQ(entry.renderPass(), TileRenderEntryPass::Selected);
}
