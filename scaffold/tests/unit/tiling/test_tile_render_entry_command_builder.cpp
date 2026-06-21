#include <gtest/gtest.h>

#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileRenderEntryCommandBuilder.h"

#include <unordered_map>

using namespace earth_engine;

namespace {

TilesetTile* findTile(
    const std::unordered_map<std::string, TilesetTile*>& tiles,
    const TileKey& key) {
    const auto it = tiles.find(TileCacheKey::forTile(key));
    return it == tiles.end() ? nullptr : it->second;
}

} // namespace

TEST(
    TileRenderEntryCommandBuilderTest,
    CountsSkippedEntriesByReason) {
    const TileKey drawnKey{"test", 0, 0, 0};
    const TileKey noDrawKey{"test", 0, 1, 0};
    const TileKey deferredKey{"test", 0, 2, 0};
    const TileKey missingRenderSelectedKey{"test", 0, 3, 0};
    const TileKey missingRenderKey{"test", 1, 3, 0};
    const TileKey missingSelectedKey{"test", 0, 4, 0};

    TilesetTile drawn(drawnKey, Rectangle{});
    TilesetTile noDraw(noDrawKey, Rectangle{});
    TilesetTile deferred(deferredKey, Rectangle{});
    TilesetTile missingRenderSelected(
        missingRenderSelectedKey,
        Rectangle{});
    std::unordered_map<std::string, TilesetTile*> tiles{
        {TileCacheKey::forTile(drawnKey), &drawn},
        {TileCacheKey::forTile(noDrawKey), &noDraw},
        {TileCacheKey::forTile(deferredKey), &deferred},
        {TileCacheKey::forTile(missingRenderSelectedKey),
         &missingRenderSelected}};

    TilePlan plan;
    auto addEntry = [&](const TileKey& selectedKey,
                        const TileKey& renderKey,
                        bool allowSynchronousMeshPrep = true) {
        TileRenderEntry entry;
        entry.selectedKey = selectedKey;
        entry.renderKey = renderKey;
        entry.allowSynchronousMeshPrep = allowSynchronousMeshPrep;
        plan.renderEntries.push_back(entry);
    };
    addEntry(drawnKey, drawnKey);
    addEntry(noDrawKey, noDrawKey);
    addEntry(deferredKey, deferredKey, false);
    addEntry(missingRenderSelectedKey, missingRenderKey);
    addEntry(missingSelectedKey, missingSelectedKey);

    Renderer renderer(nullptr);
    RenderCommandList commands;
    const TileRenderEntryCommandStats stats =
        TileRenderEntryCommandBuilder::build(
            plan,
            TileRenderEntryPass::Selected,
            7,
            renderer,
            commands,
            [&tiles](const TileKey& key) {
                return findTile(tiles, key);
            },
            [](const TileKey& key) {
                return TileCacheKey::forTile(key);
            },
            [](const std::string&) {},
            [&drawnKey](Renderer&,
                        TilesetTile& tile,
                        RenderCommandList& outCommands,
                        float,
                        bool,
                        const std::optional<std::array<float, 4>>&) {
                if (tile.key == drawnKey) {
                    RenderCommand command;
                    command.domain = RenderCommandKind::SurfaceTile;
                    outCommands.push_back(std::move(command));
                }
            });

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(stats.plannedEntries, 5);
    EXPECT_EQ(stats.ensuredTiles, 4);
    EXPECT_EQ(stats.drawAttempts, 1);
    EXPECT_EQ(stats.missingSelectedTiles, 1);
    EXPECT_EQ(stats.missingRenderTiles, 1);
    EXPECT_EQ(stats.deferredEntries, 1);
    EXPECT_EQ(stats.missedDrawEntries, 1);
}

TEST(
    TileRenderEntryCommandBuilderTest,
    KeepsSelectedAndRenderTilesActiveForFallback) {
    const TileKey selectedKey{"test", 1, 0, 0};
    const TileKey renderKey{"test", 0, 0, 0};
    TilesetTile selected(selectedKey, Rectangle{});
    TilesetTile render(renderKey, Rectangle{});
    std::unordered_map<std::string, TilesetTile*> tiles{
        {TileCacheKey::forTile(selectedKey), &selected},
        {TileCacheKey::forTile(renderKey), &render}};

    TilePlan plan;
    TileRenderEntry entry;
    entry.selectedKey = selectedKey;
    entry.renderKey = renderKey;
    entry.usesAncestorFallback = true;
    entry.surfaceClipEnabled = true;
    entry.surfaceClipUv = {0.0f, 0.5f, 0.5f, 0.5f};
    plan.renderEntries.push_back(entry);

    std::vector<std::string> ineligibleKeys;
    Renderer renderer(nullptr);
    RenderCommandList commands;
    const TileRenderEntryCommandStats stats =
        TileRenderEntryCommandBuilder::build(
            plan,
            TileRenderEntryPass::Selected,
            11,
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
            [&renderKey](Renderer&,
                         TilesetTile& tile,
                         RenderCommandList& outCommands,
                         float,
                         bool,
                         const std::optional<std::array<float, 4>>& clipUv) {
                if (tile.key == renderKey && clipUv) {
                    RenderCommand command;
                    command.domain = RenderCommandKind::SurfaceTile;
                    command.surfaceClipEnabled = 1.0f;
                    command.surfaceClipUv = *clipUv;
                    outCommands.push_back(std::move(command));
                }
            });

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(stats.plannedEntries, 1);
    EXPECT_EQ(stats.drawAttempts, 1);
    EXPECT_EQ(selected.lastUsedFrame(), 11u);
    EXPECT_EQ(render.lastUsedFrame(), 11u);
    EXPECT_EQ(selected.referenceCount(), 0);
    EXPECT_EQ(render.referenceCount(), 1);
    ASSERT_EQ(ineligibleKeys.size(), 2u);
    EXPECT_EQ(ineligibleKeys[0], TileCacheKey::forTile(selectedKey));
    EXPECT_EQ(ineligibleKeys[1], TileCacheKey::forTile(renderKey));
    EXPECT_NE(
        commands.front().stableKey.find(
            "clip:" + TileCacheKey::forTile(selectedKey)),
        std::string::npos);
}

TEST(TileRenderEntryCommandBuilderTest, BuildsFadingEntriesOnlyInFadePass) {
    const TileKey fadingKey{"test", 0, 0, 0};
    TilesetTile fading(fadingKey, Rectangle{});

    TilePlan plan;
    TileRenderEntry entry;
    entry.selectedKey = fadingKey;
    entry.renderKey = fadingKey;
    entry.reason = TileRenderEntryReason::FadingOut;
    entry.selectedThisFrame = false;
    entry.opacity = 0.4f;
    plan.renderEntries.push_back(entry);

    Renderer renderer(nullptr);
    RenderCommandList commands;
    auto ensureTile = [&fading](const TileKey& key) -> TilesetTile* {
        return key == fading.key ? &fading : nullptr;
    };
    auto cacheKey = [](const TileKey& key) {
        return TileCacheKey::forTile(key);
    };
    auto markIneligible = [](const std::string&) {};
    float submittedOpacity = 0.0f;
    auto buildCommand = [&submittedOpacity](Renderer&,
                                            TilesetTile&,
                                            RenderCommandList& outCommands,
                                            float opacity,
                                            bool,
                                            const std::optional<
                                                std::array<float, 4>>&) {
        submittedOpacity = opacity;
        RenderCommand command;
        command.domain = RenderCommandKind::SurfaceTile;
        outCommands.push_back(std::move(command));
    };

    const TileRenderEntryCommandStats selectedStats =
        TileRenderEntryCommandBuilder::build(
            plan,
            TileRenderEntryPass::Selected,
            12,
            renderer,
            commands,
            ensureTile,
            cacheKey,
            markIneligible,
            buildCommand);

    EXPECT_TRUE(commands.empty());
    EXPECT_EQ(selectedStats.plannedEntries, 0);

    const TileRenderEntryCommandStats fadeStats =
        TileRenderEntryCommandBuilder::build(
            plan,
            TileRenderEntryPass::Fading,
            12,
            renderer,
            commands,
            ensureTile,
            cacheKey,
            markIneligible,
            buildCommand);

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(fadeStats.plannedEntries, 1);
    EXPECT_EQ(fadeStats.drawAttempts, 1);
    EXPECT_NEAR(submittedOpacity, 0.4f, 1e-6f);
    EXPECT_EQ(fading.lastUsedFrame(), 12u);
    EXPECT_EQ(fading.referenceCount(), 1);
}
