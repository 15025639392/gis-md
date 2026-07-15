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
    selectedEntry.selectedTile = findTile(tiles, selectedKey);
    selectedEntry.renderTile = selectedEntry.selectedTile;
    plan.renderEntries.push_back(selectedEntry);

    TileRenderEntry fadingEntry;
    fadingEntry.selectedKey = fadingKey;
    fadingEntry.renderKey = fadingKey;
    fadingEntry.selectedTile = findTile(tiles, fadingKey);
    fadingEntry.renderTile = fadingEntry.selectedTile;
    fadingEntry.reason = TileRenderEntryReason::FadingOut;
    fadingEntry.selectedThisFrame = false;
    fadingEntry.opacity = 0.35f;
    plan.renderEntries.push_back(fadingEntry);

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
            const std::optional<std::array<float, 4>>&) {
            submittedOpacities.push_back(opacity);
            RenderCommand command;
            command.kind = RenderCommandKind::SurfaceTile;
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
    ASSERT_EQ(ineligibleKeys.size(), 4u);
    EXPECT_EQ(ineligibleKeys[0], TileCacheKey::forTile(selectedKey));
    EXPECT_EQ(ineligibleKeys[1], TileCacheKey::forTile(deferredKey));
    EXPECT_EQ(ineligibleKeys[2], TileCacheKey::forTile(missingSelectedKey));
    EXPECT_EQ(ineligibleKeys[3], TileCacheKey::forTile(fadingKey));
    EXPECT_TRUE(trimRasterCachesCalled);
    EXPECT_TRUE(unloadCachedBytesCalled);
}

TEST(
    TileRenderFrameBuilderTest,
    ProtectsVisibleAndFadingTilesWithoutRenderEntriesBeforeUnloading) {
    const TileKey readyKey{"test", 0, 0, 0};
    const TileKey blockedKey{"test", 0, 1, 0};
    const TileKey fadingKey{"test", 0, 2, 0};
    const TileKey expiredFadeKey{"test", 0, 3, 0};

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    addTile(tiles, readyKey);
    addTile(tiles, blockedKey);
    addTile(tiles, fadingKey);
    addTile(tiles, expiredFadeKey);

    TilePlan plan;
    plan.visibleTiles = {readyKey, blockedKey};
    plan.tilesFadingOut = {
        TileTransition{fadingKey, 0.4f, 1},
        TileTransition{expiredFadeKey, 0.001f, 1}};
    plan.tilesToRenderThisFrame = {
        findTile(tiles, readyKey),
        findTile(tiles, blockedKey)};
    plan.tilesFadingOutThisFrame = {
        findTile(tiles, fadingKey)};
    TileRenderEntry readyEntry;
    readyEntry.selectedKey = readyKey;
    readyEntry.renderKey = readyKey;
    readyEntry.selectedTile = findTile(tiles, readyKey);
    readyEntry.renderTile = readyEntry.selectedTile;
    plan.renderEntries.push_back(readyEntry);

    std::vector<ActivatedRasterOverlay*> overlays;
    std::vector<TileRenderReference> references;
    std::unordered_set<std::string> ineligibleKeys;
    bool unloadSawProtectedPlan = false;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TileRenderFrameBuilder::build(
        TileRenderFrameBuildInput{
            plan,
            overlays,
            31,
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
            ineligibleKeys.insert(key);
        },
        [&references](
            TilesetTile* tile,
            std::string cacheKey,
            bool countedReference) {
            references.push_back(
                TileRenderReference{
                    tile,
                    std::move(cacheKey),
                    countedReference});
        },
        [](Renderer&,
           TilesetTile&,
           RenderCommandList&,
           float,
           bool,
           const std::optional<std::array<float, 4>>&) {},
        [](bool) {},
        []() {
            return true;
        },
        [&]() {
            TilesetTile* blocked = findTile(tiles, blockedKey);
            TilesetTile* fading = findTile(tiles, fadingKey);
            TilesetTile* expiredFade = findTile(tiles, expiredFadeKey);
            ASSERT_NE(blocked, nullptr);
            ASSERT_NE(fading, nullptr);
            ASSERT_NE(expiredFade, nullptr);
            unloadSawProtectedPlan =
                blocked->referenceCount() == 1 &&
                fading->referenceCount() == 1 &&
                expiredFade->referenceCount() == 0 &&
                ineligibleKeys.count(
                    TileCacheKey::forTile(blockedKey)) == 1 &&
                ineligibleKeys.count(
                    TileCacheKey::forTile(fadingKey)) == 1;
        });

    EXPECT_TRUE(unloadSawProtectedPlan);
    EXPECT_EQ(references.size(), 3u);
    EXPECT_EQ(findTile(tiles, readyKey)->referenceCount(), 1);
    EXPECT_EQ(findTile(tiles, blockedKey)->lastUsedFrame(), 31u);
    EXPECT_EQ(findTile(tiles, fadingKey)->lastUsedFrame(), 31u);
    EXPECT_EQ(findTile(tiles, expiredFadeKey)->lastUsedFrame(), 0u);

    TileRenderReferenceReleaser::release(
        references,
        [](const TilesetTile*, const std::string&) {});
    EXPECT_EQ(findTile(tiles, readyKey)->referenceCount(), 0);
    EXPECT_EQ(findTile(tiles, blockedKey)->referenceCount(), 0);
    EXPECT_EQ(findTile(tiles, fadingKey)->referenceCount(), 0);
}
