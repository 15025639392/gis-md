#include <gtest/gtest.h>

#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileRenderFrameBuilder.h"
#include "earth_engine/tiling/TileRenderReferenceReleaser.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    AggregatesSelectedDeferredAndMissingRenderEntryStats) {
    const TileKey selectedKey{"test", 0, 0, 0};
    const TileKey deferredKey{"test", 0, 2, 0};
    const TileKey missingSelectedKey{"test", 0, 3, 0};
    const TileKey missingRenderKey{"test", 1, 3, 0};

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    addTile(tiles, selectedKey);
    addTile(tiles, deferredKey);
    addTile(tiles, missingSelectedKey);

    TilePlan plan;
    TileRenderEntry selectedEntry;
    selectedEntry.selectedKey = selectedKey;
    selectedEntry.renderKey = selectedKey;
    selectedEntry.selectedTile = findTile(tiles, selectedKey);
    selectedEntry.renderTile = selectedEntry.selectedTile;
    plan.renderEntries.push_back(selectedEntry);

    TileRenderEntry deferredEntry;
    deferredEntry.selectedKey = deferredKey;
    deferredEntry.renderKey = deferredKey;
    deferredEntry.selectedTile = findTile(tiles, deferredKey);
    deferredEntry.renderTile = deferredEntry.selectedTile;
    deferredEntry.allowSynchronousMeshPrep = false;
    plan.renderEntries.push_back(deferredEntry);

    TileRenderEntry missingRenderEntry;
    missingRenderEntry.selectedKey = missingSelectedKey;
    missingRenderEntry.renderKey = missingRenderKey;
    missingRenderEntry.selectedTile =
        findTile(tiles, missingSelectedKey);
    plan.renderEntries.push_back(missingRenderEntry);

    std::vector<ActivatedRasterOverlay*> overlays;
    bool trimRasterCachesCalled = false;
    bool unloadCachedBytesCalled = false;
    std::vector<std::string> ineligibleKeys;
    std::vector<float> submittedOpacities;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TileRenderFrameBuilder::build(
        TileRenderFrameBuildInput{
            plan,
            overlays,
            23,
            Vec3{6378137.0, 0.0, 0.0},
            {},
            0,
            false,
            false},
        renderer,
        commands,
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [&ineligibleKeys](const std::string& key) {
            ineligibleKeys.push_back(key);
        },
        [](TilesetTile*, std::string, bool) {},
        [&submittedOpacities](
            Renderer&,
            TilesetTile&,
            RenderCommandList& outCommands,
            float opacity,
            bool,
            const std::optional<std::array<float, 4>>&,
            const TilesetTile*) {
            submittedOpacities.push_back(opacity);
            RenderCommand command;
            command.kind = RenderCommandKind::GltfPrimitive;
            outCommands.push_back(std::move(command));
        },
        [&trimRasterCachesCalled](bool cachePressure) {
            EXPECT_FALSE(cachePressure);
            trimRasterCachesCalled = true;
        },
        []() { return true; },
        [&unloadCachedBytesCalled]() {
            unloadCachedBytesCalled = true;
        });

    ASSERT_EQ(commands.size(), 1u);
    ASSERT_EQ(submittedOpacities.size(), 1u);
    EXPECT_NEAR(submittedOpacities[0], 1.0f, 1e-6f);

    EXPECT_EQ(plan.renderEntryPlannedCommandCount, 3);
    EXPECT_EQ(plan.renderEntrySelectedPlannedCommandCount, 3);
    EXPECT_EQ(plan.renderEntryCommandDrawCount, 1);
    EXPECT_EQ(plan.renderEntrySelectedCommandDrawCount, 1);
    EXPECT_EQ(plan.renderEntryCommandMissedDrawCount, 0);
    EXPECT_EQ(plan.renderEntrySelectedCommandMissedDrawCount, 0);
    EXPECT_EQ(plan.renderEntryCommandMissingSelectedCount, 0);
    EXPECT_EQ(plan.renderEntryCommandMissingRenderCount, 1);
    EXPECT_EQ(plan.renderEntryCommandDeferredCount, 1);
    EXPECT_EQ(plan.renderEntrySelectedCommandDeferredCount, 1);

    EXPECT_EQ(findTile(tiles, selectedKey)->lastUsedFrame(), 23u);
    EXPECT_EQ(findTile(tiles, deferredKey)->lastUsedFrame(), 23u);
    EXPECT_EQ(findTile(tiles, missingSelectedKey)->lastUsedFrame(), 23u);
    ASSERT_EQ(ineligibleKeys.size(), 3u);
    EXPECT_EQ(ineligibleKeys[0], TileCacheKey::forTile(selectedKey));
    EXPECT_EQ(ineligibleKeys[1], TileCacheKey::forTile(deferredKey));
    EXPECT_EQ(ineligibleKeys[2], TileCacheKey::forTile(missingSelectedKey));
    EXPECT_TRUE(trimRasterCachesCalled);
    EXPECT_TRUE(unloadCachedBytesCalled);
}
