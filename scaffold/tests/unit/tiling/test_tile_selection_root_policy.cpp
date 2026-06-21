#include <gtest/gtest.h>

#include "earth_engine/tiling/TileSelectionRootPolicy.h"

using namespace earth_engine;

TEST(TileSelectionRootPolicyTest, ExplicitContentRootsTakePriority) {
    const std::vector<TileKey> explicitRoots{
        TileKey{"content", 2, 4, 6},
        TileKey{"content", 3, 5, 7},
    };

    EXPECT_EQ(
        TileSelectionRootPolicy::chooseRoots(
            "Geographic-TMS",
            explicitRoots),
        explicitRoots);
}

TEST(TileSelectionRootPolicyTest, GeographicTmsUsesTwoLevelZeroRoots) {
    const std::vector<TileKey> roots =
        TileSelectionRootPolicy::chooseRoots("Geographic-TMS", {});

    ASSERT_EQ(roots.size(), 2u);
    EXPECT_EQ(roots[0], (TileKey{"Geographic-TMS", 0, 0, 0}));
    EXPECT_EQ(roots[1], (TileKey{"Geographic-TMS", 0, 1, 0}));
}

TEST(TileSelectionRootPolicyTest, OpenGlobusEarthUsesThreeRoots) {
    const std::vector<TileKey> roots =
        TileSelectionRootPolicy::chooseRoots("OpenGlobus-Earth", {});

    ASSERT_EQ(roots.size(), 3u);
    EXPECT_EQ(roots[0], (TileKey{"OpenGlobus-Earth", 0, 0, 0}));
    EXPECT_EQ(roots[1], (TileKey{"OpenGlobus-Earth", 0, 0, 1}));
    EXPECT_EQ(roots[2], (TileKey{"OpenGlobus-Earth", 0, 0, 2}));
}

TEST(TileSelectionRootPolicyTest, UnknownSchemeUsesOneDefaultRoot) {
    const std::vector<TileKey> roots =
        TileSelectionRootPolicy::chooseRoots("custom", {});

    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots[0], (TileKey{"custom", 0, 0, 0}));
}
