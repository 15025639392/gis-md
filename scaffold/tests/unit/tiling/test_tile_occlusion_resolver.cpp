#include <gtest/gtest.h>

#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileOcclusionResolver.h"

#include <unordered_map>

using namespace earth_engine;

namespace {

void setTileKey(TilesetTile& tile, const TileKey& key) {
    tile.key = key;
}

} // namespace

TEST(TileOcclusionResolverTest, OccludedChildrenOccludeVisibleParent) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childAKey{"Geographic-TMS", 1, 0, 0};
    const TileKey childBKey{"Geographic-TMS", 1, 1, 0};

    TilesetTile root;
    TilesetTile childA;
    TilesetTile childB;
    setTileKey(root, rootKey);
    setTileKey(childA, childAKey);
    setTileKey(childB, childBKey);
    root.children = {&childA, &childB};

    std::unordered_map<TileKey, TileOcclusionState> states;
    states[rootKey] = TileOcclusionState::NotOccluded;
    states[childAKey] = TileOcclusionState::Occluded;
    states[childBKey] = TileOcclusionState::Occluded;

    const TileOcclusionState result = TileOcclusionResolver::check(
        root,
        [&states](const TilesetTile& tile) {
            auto it = states.find(tile.key);
            return it == states.end()
                ? TileOcclusionState::NotOccluded
                : it->second;
        });

    EXPECT_EQ(result, TileOcclusionState::Occluded);
}
