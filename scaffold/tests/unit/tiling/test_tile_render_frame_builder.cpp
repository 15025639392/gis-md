#include <gtest/gtest.h>

#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileRenderFrameBuilder.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace earth_engine;

namespace {

TilesetTile* findTile(
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    const TileKey& key) {
    const auto it = tiles.find(TileCacheKey::forTile(key));
    return it == tiles.end() ? nullptr : it->second.get();
}

void addTile(
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    const TileKey& key) {
    tiles.emplace(TileCacheKey::forTile(key), std::make_unique<TilesetTile>(
        key,
        Rectangle{}));
}

} // namespace

TEST(
    TileRenderFrameBuilderTest,
    AggregatesSelectedFadingDeferredAndMissingRenderEntryStats) {
    const TileKey selectedKey{"test", 0, 0, 0};
    const TileKey fadingKey{"test", 0, 1, 0};
    const TileKey deferredKey{"test", 0, 2, 0};
    const TileKey missingSelectedKey{"test", 0, 3, 0};
    const TileKey missingRenderKey{"test", 1, 3, 0};

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    addTile(tiles, selectedKey);
    addTile(tiles, fadingKey);
    addTile(tiles, deferredKey);
    addTile(tiles, missingSelectedKey);

    TilePlan plan;
    TileRenderEntry selectedEntry;
    selectedEntry.selectedKey = selectedKey;
    selectedEntry.renderKey = selectedKey;
    plan.renderEntries.push_back(selectedEntry);

    TileRenderEntry fadingEntry;
    fadingEntry.selectedKey = fadingKey;
    fadingEntry.renderKey = fadingKey;
    fadingEntry.reason = TileRenderEntryReason::FadingOut;
    fadingEntry.selectedThisFrame = false;
    fadingEntry.opacity = 0.35f;
    plan.renderEntries.push_back(fadingEntry);

    TileRenderEntry deferredEntry;
    deferredEntry.selectedKey = deferredKey;
    deferredEntry.renderKey = deferredKey;
    deferredEntry.allowSynchronousMeshPrep = false;
    plan.renderEntries.push_back(deferredEntry);

    TileRenderEntry missingRenderEntry;
    missingRenderEntry.selectedKey = missingSelectedKey;
    missingRenderEntry.renderKey = missingRenderKey;
    plan.renderEntries.push_back(missingRenderEntry);

    TileUnloadQueue unloadQueue;
    std::vector<ActivatedRasterOverlay*> overlays;
    bool cacheBytesDirty = true;
    bool updateTotalBytesCalled = false;
    bool unloadCachedBytesCalled = false;
    std::vector<std::string> ineligibleKeys;
    std::vector<float> submittedOpacities;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TileRenderFrameBuilder::build(
        TileRenderFrameBuildInput{
            plan,
            tiles,
            unloadQueue,
            overlays,
            cacheBytesDirty,
            23,
            Vec3{6378137.0, 0.0, 0.0},
            {},
            0,
            false,
            false,
            2048,
            1024},
        renderer,
        commands,
        [&tiles](const TileKey& key) {
            return findTile(tiles, key);
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [&ineligibleKeys](const std::string& key) {
            ineligibleKeys.push_back(key);
        },
        [&submittedOpacities](
            Renderer&,
            TilesetTile&,
            RenderCommandList& outCommands,
            float opacity,
            bool,
            const std::optional<std::array<float, 4>>&) {
            submittedOpacities.push_back(opacity);
            RenderCommand command;
            command.domain = RenderCommandKind::SurfaceTile;
            outCommands.push_back(std::move(command));
        },
        [](const std::vector<TileFrameInactiveEntry>&) {},
        [](const std::string&) {},
        [&updateTotalBytesCalled]() {
            updateTotalBytesCalled = true;
        },
        [&unloadCachedBytesCalled]() {
            unloadCachedBytesCalled = true;
        });

    ASSERT_EQ(commands.size(), 2u);
    ASSERT_EQ(submittedOpacities.size(), 2u);
    EXPECT_NEAR(submittedOpacities[0], 1.0f, 1e-6f);
    EXPECT_NEAR(submittedOpacities[1], 0.35f, 1e-6f);

    EXPECT_EQ(plan.renderEntryPlannedCommandCount, 4);
    EXPECT_EQ(plan.renderEntrySelectedPlannedCommandCount, 3);
    EXPECT_EQ(plan.renderEntryFadingPlannedCommandCount, 1);
    EXPECT_EQ(plan.renderEntryCommandDrawCount, 2);
    EXPECT_EQ(plan.renderEntrySelectedCommandDrawCount, 1);
    EXPECT_EQ(plan.renderEntryFadingCommandDrawCount, 1);
    EXPECT_EQ(plan.renderEntryCommandMissedDrawCount, 0);
    EXPECT_EQ(plan.renderEntrySelectedCommandMissedDrawCount, 0);
    EXPECT_EQ(plan.renderEntryFadingCommandMissedDrawCount, 0);
    EXPECT_EQ(plan.renderEntryCommandMissingSelectedCount, 0);
    EXPECT_EQ(plan.renderEntryCommandMissingRenderCount, 1);
    EXPECT_EQ(plan.renderEntryCommandDeferredCount, 1);
    EXPECT_EQ(plan.renderEntrySelectedCommandDeferredCount, 1);
    EXPECT_EQ(plan.renderEntryFadingCommandDeferredCount, 0);

    EXPECT_EQ(findTile(tiles, selectedKey)->lastUsedFrame(), 23u);
    EXPECT_EQ(findTile(tiles, fadingKey)->lastUsedFrame(), 23u);
    EXPECT_EQ(findTile(tiles, deferredKey)->lastUsedFrame(), 23u);
    EXPECT_EQ(findTile(tiles, missingSelectedKey)->lastUsedFrame(), 23u);
    ASSERT_EQ(ineligibleKeys.size(), 7u);
    EXPECT_EQ(ineligibleKeys[0], TileCacheKey::forTile(selectedKey));
    EXPECT_EQ(ineligibleKeys[1], TileCacheKey::forTile(selectedKey));
    EXPECT_EQ(ineligibleKeys[2], TileCacheKey::forTile(deferredKey));
    EXPECT_EQ(ineligibleKeys[3], TileCacheKey::forTile(deferredKey));
    EXPECT_EQ(ineligibleKeys[4], TileCacheKey::forTile(missingSelectedKey));
    EXPECT_EQ(ineligibleKeys[5], TileCacheKey::forTile(fadingKey));
    EXPECT_EQ(ineligibleKeys[6], TileCacheKey::forTile(fadingKey));
    EXPECT_TRUE(updateTotalBytesCalled);
    EXPECT_TRUE(unloadCachedBytesCalled);
    EXPECT_FALSE(cacheBytesDirty);
}
