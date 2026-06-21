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
                    command.kind = RenderCommandKind::SurfaceTile;
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
