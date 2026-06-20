#include <gtest/gtest.h>

#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileChildMaterializer.h"

#include <memory>
#include <string>
#include <unordered_map>

using namespace earth_engine;

namespace {

std::string cacheKeyFor(const TileKey& key) {
    return key.schemeId + ":" +
           std::to_string(key.z) + ":" +
           std::to_string(key.x) + ":" +
           std::to_string(key.y);
}

} // namespace

TEST(TileChildMaterializerTest, AnyAvailableTerrainChildCreatesFullQuadLikeCesiumNative) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle{});
    parent.geometricError = 100.0;
    parent.refine = TileRefine::Add;
    parent.content.renderContent.setTerrainHeightRange(-10.0, 90.0);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto ensure = [&tiles](const TileKey& key) -> TilesetTile* {
        const std::string cacheKey = cacheKeyFor(key);
        auto it = tiles.find(cacheKey);
        if (it == tiles.end()) {
            it = tiles.emplace(
                cacheKey,
                std::make_unique<TilesetTile>(key, Rectangle{})).first;
        }
        return it->second.get();
    };
    auto availability = [](const TileKey& key) {
        return key.x == 0 && key.y == 0
            ? TileAvailabilityState::Available
            : TileAvailabilityState::NotAvailable;
    };

    const bool changed = TileChildMaterializer::materializeTerrainChildren(
        parent,
        2,
        availability,
        ensure);

    ASSERT_TRUE(changed);
    ASSERT_EQ(4u, parent.children.size());

    TilesetTile* sw = parent.children[0];
    TilesetTile* se = parent.children[1];
    TilesetTile* nw = parent.children[2];
    TilesetTile* ne = parent.children[3];

    EXPECT_FALSE(sw->content.upsampledFromParent);
    EXPECT_TRUE(se->content.upsampledFromParent);
    EXPECT_TRUE(nw->content.upsampledFromParent);
    EXPECT_TRUE(ne->content.upsampledFromParent);

    EXPECT_DOUBLE_EQ(50.0, sw->geometricError);
    EXPECT_DOUBLE_EQ(50.0, se->geometricError);
    EXPECT_EQ(TileRefine::Add, sw->refine);
    EXPECT_EQ(TileRefine::Add, se->refine);
    EXPECT_TRUE(sw->content.renderContent.hasTerrainHeightRange());
    EXPECT_TRUE(se->content.renderContent.hasTerrainHeightRange());
    EXPECT_DOUBLE_EQ(-10.0, sw->content.renderContent.terrainMinimumHeight());
    EXPECT_DOUBLE_EQ(90.0, se->content.renderContent.terrainMaximumHeight());
}
