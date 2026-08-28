#include <gtest/gtest.h>

#include "earth_engine/tiling/DirectRasterMapping.h"
#include "earth_engine/tiling/TileSelectionChildTraversal.h"
#include "earth_engine/tiling/TilesetTile.h"

#include <vector>

using namespace earth_engine;

TEST(TileSelectionChildTraversalTest, VisitsNonNullChildrenAndAggregates) {
    TilesetTile parent(
        TileKey{"test", 0, 0, 0},
        Rectangle{-1.0, -1.0, 1.0, 1.0});
    TilesetTile childA(
        TileKey{"test", 1, 0, 0},
        Rectangle{-1.0, -1.0, 0.0, 0.0},
        &parent);
    TilesetTile childB(
        TileKey{"test", 1, 1, 0},
        Rectangle{0.0, -1.0, 1.0, 0.0},
        &parent);
    parent.children.push_back(&childA);
    parent.children.push_back(nullptr);
    parent.children.push_back(&childB);

    std::vector<TileKey> visited;
    const TileTraversalDetails details =
        TileSelectionChildTraversal::visitChildren(
            parent.children,
            [&visited](TilesetTile& child) {
                visited.push_back(child.key);
                return child.key.x == 0
                    ? TileTraversalDetailsPolicy::forSingleTile(true, true)
                    : TileTraversalDetailsPolicy::forSingleTile(false, false);
            });

    ASSERT_EQ(visited.size(), 2u);
    EXPECT_EQ(visited[0], childA.key);
    EXPECT_EQ(visited[1], childB.key);
    EXPECT_FALSE(details.allAreRenderable);
    EXPECT_TRUE(details.anyWereRenderedLastFrame);
    EXPECT_EQ(details.notYetRenderableCount, 1u);
}
