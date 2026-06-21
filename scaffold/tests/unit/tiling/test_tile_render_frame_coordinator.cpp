#include <gtest/gtest.h>

#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileRenderFrameCoordinator.h"

#include <array>
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

} // namespace

TEST(
    TileRenderFrameCoordinatorTest,
    ForwardsFallbackRenderCommandContextAndCacheKeys) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 1, 0};
    auto parent = std::make_unique<TilesetTile>(
        parentKey,
        Rectangle{0.0, 0.0, 2.0, 2.0});
    auto child = std::make_unique<TilesetTile>(
        childKey,
        Rectangle{1.0, 1.0, 2.0, 2.0},
        parent.get());

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    tiles.emplace(TileCacheKey::forTile(parentKey), std::move(parent));
    tiles.emplace(TileCacheKey::forTile(childKey), std::move(child));

    TilePlan plan;
    TileRenderEntry entry;
    entry.selectedKey = childKey;
    entry.renderKey = parentKey;
    entry.reason = TileRenderEntryReason::AncestorFallback;
    entry.opacity = 0.65f;
    entry.usesAncestorFallback = true;
    entry.allowSynchronousMeshPrep = true;
    entry.surfaceClipEnabled = true;
    entry.surfaceClipUv = {0.5f, 0.0f, 0.5f, 0.5f};
    plan.renderEntries.push_back(entry);

    TileUnloadQueue unloadQueue;
    std::vector<ActivatedRasterOverlay*> overlays;
    bool cacheBytesDirty = false;
    std::vector<std::string> ineligibleKeys;
    bool updateTotalBytesCalled = false;
    bool unloadCachedBytesCalled = false;
    bool buildCommandCalled = false;
    float forwardedOpacity = 0.0f;
    bool forwardedAllowSynchronousMeshPrep = false;
    std::optional<std::array<float, 4>> forwardedClipUv;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TileRenderFrameCoordinator::run(
        TileRenderFrameCoordinatorInput{
            plan,
            tiles,
            unloadQueue,
            overlays,
            cacheBytesDirty,
            41,
            Vec3{6378137.0, 0.0, 0.0},
            {},
            0,
            false,
            true,
            512,
            1024},
        renderer,
        commands,
        [&tiles](const TileKey& key) {
            return findTile(tiles, key);
        },
        [&ineligibleKeys](const std::string& cacheKey) {
            ineligibleKeys.push_back(cacheKey);
        },
        [&](Renderer&,
            TilesetTile& tile,
            RenderCommandList& outCommands,
            float transitionOpacity,
            bool allowSynchronousMeshPrep,
            const std::optional<std::array<float, 4>>& surfaceClipUv) {
            buildCommandCalled = true;
            EXPECT_EQ(tile.key, parentKey);
            forwardedOpacity = transitionOpacity;
            forwardedAllowSynchronousMeshPrep = allowSynchronousMeshPrep;
            forwardedClipUv = surfaceClipUv;
            RenderCommand command;
            command.domain = RenderCommandKind::SurfaceTile;
            outCommands.push_back(std::move(command));
        },
        [](const std::string&) {},
        [&updateTotalBytesCalled]() {
            updateTotalBytesCalled = true;
        },
        [&unloadCachedBytesCalled]() {
            unloadCachedBytesCalled = true;
        });

    EXPECT_TRUE(buildCommandCalled);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_NEAR(forwardedOpacity, 0.65f, 1e-6f);
    EXPECT_TRUE(forwardedAllowSynchronousMeshPrep);
    ASSERT_TRUE(forwardedClipUv.has_value());
    EXPECT_NEAR((*forwardedClipUv)[0], 0.5f, 1e-6f);
    EXPECT_NEAR((*forwardedClipUv)[1], 0.0f, 1e-6f);
    EXPECT_NEAR((*forwardedClipUv)[2], 0.5f, 1e-6f);
    EXPECT_NEAR((*forwardedClipUv)[3], 0.5f, 1e-6f);
    ASSERT_EQ(ineligibleKeys.size(), 2u);
    EXPECT_EQ(ineligibleKeys[0], TileCacheKey::forTile(childKey));
    EXPECT_EQ(ineligibleKeys[1], TileCacheKey::forTile(parentKey));
    EXPECT_EQ(findTile(tiles, childKey)->lastUsedFrame(), 41u);
    EXPECT_EQ(findTile(tiles, parentKey)->lastUsedFrame(), 41u);
    EXPECT_EQ(findTile(tiles, parentKey)->referenceCount(), 1);
    EXPECT_FALSE(updateTotalBytesCalled);
    EXPECT_FALSE(unloadCachedBytesCalled);
}
