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

TEST(TileOcclusionResolverTest, VisibleChildKeepsParentVisible) {
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
    states[childAKey] = TileOcclusionState::NotOccluded;
    states[childBKey] = TileOcclusionState::Occluded;

    const TileOcclusionState result = TileOcclusionResolver::check(
        root,
        [&states](const TilesetTile& tile) {
            auto it = states.find(tile.key);
            return it == states.end()
                ? TileOcclusionState::NotOccluded
                : it->second;
        });

    EXPECT_EQ(result, TileOcclusionState::NotOccluded);
}

TEST(TileOcclusionResolverTest, MissingChildKeepsParentVisible) {
    TilesetTile root;
    setTileKey(root, TileKey{"Geographic-TMS", 0, 0, 0});
    root.children.push_back(nullptr);

    int checkedTiles = 0;
    const TileOcclusionState result = TileOcclusionResolver::check(
        root,
        [&checkedTiles](const TilesetTile&) {
            ++checkedTiles;
            return TileOcclusionState::NotOccluded;
        });

    EXPECT_EQ(result, TileOcclusionState::NotOccluded);
    EXPECT_EQ(checkedTiles, 1);
}

TEST(TileOcclusionResolverTest, TerminalParentStateSkipsChildren) {
    auto checkTerminalState = [](TileOcclusionState state) {
        TilesetTile root;
        TilesetTile child;
        setTileKey(root, TileKey{"Geographic-TMS", 0, 0, 0});
        setTileKey(child, TileKey{"Geographic-TMS", 1, 0, 0});
        root.children.push_back(&child);

        int checkedTiles = 0;
        const TileOcclusionState result = TileOcclusionResolver::check(
            root,
            [state, &checkedTiles](const TilesetTile&) {
                ++checkedTiles;
                return state;
            });

        EXPECT_EQ(result, state);
        EXPECT_EQ(checkedTiles, 1);
    };

    checkTerminalState(TileOcclusionState::Occluded);
    checkTerminalState(TileOcclusionState::OcclusionUnavailable);
}

TEST(TileOcclusionResolverTest, UnconditionalChildSkipsAggregation) {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};

    TilesetTile root;
    TilesetTile child;
    setTileKey(root, rootKey);
    setTileKey(child, childKey);
    child.unconditionallyRefine = true;
    root.children.push_back(&child);

    std::unordered_map<TileKey, TileOcclusionState> states;
    states[rootKey] = TileOcclusionState::NotOccluded;
    states[childKey] = TileOcclusionState::Occluded;

    const TileOcclusionState result = TileOcclusionResolver::check(
        root,
        [&states](const TilesetTile& tile) {
            auto it = states.find(tile.key);
            return it == states.end()
                ? TileOcclusionState::NotOccluded
                : it->second;
        });

    EXPECT_EQ(result, TileOcclusionState::NotOccluded);
}
