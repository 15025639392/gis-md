#include <gtest/gtest.h>

#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/renderer/RenderCommand.h"
#include "earth_engine/tiling/RenderedTerrainSurfaceSampler.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TilesetTile.h"

#include <memory>

using namespace earth_engine;

namespace {

constexpr const char* kScheme = "Geographic-TMS";

std::unique_ptr<DecodedHeightmap> makeRampHeightmap(float scale = 1.0f) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 3;
    heightmap->stagedHeights = {
        0.0f, 10.0f, 20.0f,
        30.0f, 40.0f, 50.0f,
        60.0f, 70.0f, 80.0f};
    for (float& height : heightmap->stagedHeights) {
        height *= scale;
    }
    heightmap->assignHeights();
    heightmap->minHeight = 0.0f;
    heightmap->maxHeight = 80.0f * scale;
    return heightmap;
}

std::unique_ptr<DecodedHeightmap> makeNonPlanarCellHeightmap() {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    // Template indices are a-c-b / b-c-d. At the cell centre, which lies on
    // the b-c diagonal, the emitted triangle surface is exactly 0. A
    // four-corner bilinear patch would incorrectly return 25.
    heightmap->stagedHeights = {0.0f, 0.0f,
                                0.0f, 100.0f};
    heightmap->assignHeights();
    heightmap->minHeight = 0.0f;
    heightmap->maxHeight = 100.0f;
    return heightmap;
}

std::unique_ptr<DecodedHeightmap> makeNonPlanarGeomorphHeightmap() {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 3;
    // Even coarse corners are 0/0/0/100, while the fine center is 0. The
    // shader/baked mesh therefore gives the center coarse=25, fine=0.
    heightmap->stagedHeights = {0.0f, 0.0f, 0.0f,
                                0.0f, 0.0f, 50.0f,
                                0.0f, 50.0f, 100.0f};
    heightmap->assignHeights();
    heightmap->minHeight = 0.0f;
    heightmap->maxHeight = 100.0f;
    return heightmap;
}

std::unique_ptr<DecodedHeightmap> makeNorthSouthRampHeightmap() {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 3;
    heightmap->stagedHeights = {0.0f, 0.0f, 0.0f,
                                50.0f, 50.0f, 50.0f,
                                100.0f, 100.0f, 100.0f};
    heightmap->assignHeights();
    heightmap->minHeight = 0.0f;
    heightmap->maxHeight = 100.0f;
    return heightmap;
}

std::unique_ptr<TilesetTile> makeTerrainTile(
    const TileScheme& scheme, const TileKey& key, float scale = 1.0f) {
    auto tile = std::make_unique<TilesetTile>(
        key, scheme.tileToRectangle(key));
    tile->content.renderContent.setTerrainRenderContent(true);
    tile->content.renderContent.setRetainedHeightmap(
        makeRampHeightmap(scale));
    return tile;
}

std::pair<double, double> pointIn(const Rectangle& bounds,
                                  double u, double v) {
    return {bounds.west() + bounds.width() * u,
            bounds.north() - bounds.height() * v};
}

} // namespace

TEST(RenderedTerrainSurfaceSamplerTest,
     DirectEntryMatchesTheSharedGpuVisibleHeightContract) {
    auto scheme = TileScheme::createGeographicTMS();
    auto tile = makeTerrainTile(*scheme, TileKey{kScheme, 7, 40, 20});
    tile->selectionFrameState.displacementGridSize = 2;
    tile->selectionFrameState.terrainMorphFactor = 0.25f;

    TilePlan plan;
    TileRenderEntry entry;
    entry.selectedKey = tile->key;
    entry.renderKey = tile->key;
    entry.selectedTile = tile.get();
    entry.renderTile = tile.get();
    plan.renderEntries.push_back(entry);

    const auto [lng, lat] = pointIn(tile->bounds, 0.37, 0.61);
    const auto source = terrain_edge::sourceOf(plan.renderEntries.front());
    const float expected = terrain_edge::renderedHeight(source, lng, lat);
    RenderedTerrainSurfaceSampler sampler(plan, *scheme);
    const auto actual = sampler.sample(lng, lat);

    ASSERT_TRUE(actual.has_value());
    EXPECT_NEAR(expected, *actual, 1e-5f);
    EXPECT_LT(*actual, 80.0f)
        << "z7 relief fade must be part of the visible-surface contract";
}

TEST(RenderedTerrainSurfaceSamplerTest,
     AncestorFallbackUsesRenderTileHeightInsideSelectedFootprint) {
    auto scheme = TileScheme::createGeographicTMS();
    auto ancestor = makeTerrainTile(*scheme, TileKey{kScheme, 7, 40, 20});
    const TileKey childKey{kScheme, 8, 80, 40};
    auto child = std::make_unique<TilesetTile>(
        childKey, scheme->tileToRectangle(childKey));

    TilePlan plan;
    TileRenderEntry entry;
    entry.selectedKey = childKey;
    entry.renderKey = ancestor->key;
    entry.reason = TileRenderEntryReason::AncestorFallback;
    entry.usesAncestorFallback = true;
    entry.surfaceClipEnabled = true;
    entry.surfaceClipUv = {0.0f, 0.0f, 0.5f, 0.5f};
    entry.selectedTile = child.get();
    entry.renderTile = ancestor.get();
    plan.renderEntries.push_back(entry);

    RenderedTerrainSurfaceSampler sampler(plan, *scheme);
    const auto [insideLng, insideLat] = pointIn(child->bounds, 0.4, 0.6);
    const auto source = terrain_edge::sourceOf(plan.renderEntries.front());
    ASSERT_TRUE(source.valid());
    const auto inside = sampler.sample(insideLng, insideLat);
    ASSERT_TRUE(inside.has_value());
    EXPECT_NEAR(terrain_edge::renderedHeight(source, insideLng, insideLat),
                *inside, 1e-5f);

    const auto [outsideLng, outsideLat] = pointIn(ancestor->bounds, 0.75, 0.75);
    EXPECT_FALSE(sampler.sample(outsideLng, outsideLat).has_value())
        << "a clipped ancestor entry may not claim the rest of its source tile";
}

TEST(RenderedTerrainSurfaceSamplerTest,
     AreaSamplerPrefiltersAndSelectedPassBeatsFadingOverlap) {
    auto scheme = TileScheme::createGeographicTMS();
    auto current = makeTerrainTile(*scheme, TileKey{kScheme, 9, 160, 80}, 1.0f);
    auto fading = makeTerrainTile(*scheme, TileKey{kScheme, 9, 160, 80}, 5.0f);
    current->selectionFrameState.displacementGridSize = 2;
    fading->selectionFrameState.displacementGridSize = 2;

    TilePlan plan;
    TileRenderEntry oldEntry;
    oldEntry.selectedKey = fading->key;
    oldEntry.renderKey = fading->key;
    oldEntry.selectedThisFrame = false;
    oldEntry.opacity = 0.9f;
    oldEntry.selectedTile = fading.get();
    oldEntry.renderTile = fading.get();
    plan.renderEntries.push_back(oldEntry);
    TileRenderEntry currentEntry = oldEntry;
    currentEntry.selectedThisFrame = true;
    currentEntry.opacity = 1.0f;
    currentEntry.selectedTile = current.get();
    currentEntry.renderTile = current.get();
    plan.renderEntries.push_back(currentEntry);

    RenderedTerrainSurfaceSampler sampler(plan, *scheme);
    const auto [lng, lat] = pointIn(current->bounds, 0.4, 0.6);
    const auto areaSampler = sampler.makeAreaSampler(current->bounds);
    ASSERT_TRUE(areaSampler);
    const auto sampled = areaSampler(lng, lat);
    ASSERT_TRUE(sampled.has_value());
    const auto currentSource = terrain_edge::sourceOf(plan.renderEntries[1]);
    EXPECT_NEAR(terrain_edge::renderedHeight(currentSource, lng, lat),
                *sampled, 1e-5f);

    const Rectangle disjoint = scheme->tileToRectangle(
        TileKey{kScheme, 9, 300, 80});
    EXPECT_FALSE(sampler.makeAreaSampler(disjoint));
}

TEST(RenderedTerrainSurfaceSamplerTest,
     SubmittedCompositionFragmentUsesSelectedKeyFootprint) {
    auto scheme = TileScheme::createGeographicTMS();
    auto source = makeTerrainTile(*scheme, TileKey{kScheme, 7, 40, 20});
    source->selectionFrameState.displacementGridSize = 2;

    TileRenderEntry fragment;
    fragment.selectedKey = TileKey{kScheme, 8, 80, 40};
    fragment.renderKey = source->key;
    fragment.selectedTile = source.get();
    fragment.renderTile = source.get();
    fragment.surfaceClipEnabled = true;
    fragment.surfaceClipUv = {0.0f, 0.0f, 0.5f, 0.5f};
    const std::vector<TileRenderEntry> submitted{fragment};

    RenderedTerrainSurfaceSampler sampler(submitted, *scheme);
    const Rectangle fragmentBounds =
        scheme->tileToRectangle(fragment.selectedKey);
    const auto [insideLng, insideLat] = pointIn(fragmentBounds, 0.5, 0.5);
    EXPECT_TRUE(sampler.sample(insideLng, insideLat).has_value());

    const auto [replacedLng, replacedLat] =
        pointIn(source->bounds, 0.75, 0.75);
    EXPECT_FALSE(sampler.sample(replacedLng, replacedLat).has_value())
        << "a composed current fragment must not cover the pending patch";
}

TEST(RenderedTerrainSurfaceSamplerTest,
     EmittedCommandOverridesPlanFadeGridAndMorph) {
    auto scheme = TileScheme::createGeographicTMS();
    auto tile = makeTerrainTile(*scheme, TileKey{kScheme, 6, 20, 10});
    tile->selectionFrameState.displacementGridSize = 2;
    tile->selectionFrameState.terrainMorphFactor = 0.25f;
    TileRenderEntry entry;
    entry.selectedKey = tile->key;
    entry.renderKey = tile->key;
    entry.selectedTile = tile.get();
    entry.renderTile = tile.get();
    const std::vector<TileRenderEntry> submitted{entry};

    RenderCommand command;
    command.terrainRenderContent = true;
    command.terrainVisibleSurfaceValid = true;
    command.terrainVisibleSelectedZ = tile->key.z;
    command.terrainVisibleSelectedX = tile->key.x;
    command.terrainVisibleSelectedY = tile->key.y;
    command.terrainVisibleRenderZ = tile->key.z;
    command.terrainVisibleRenderX = tile->key.x;
    command.terrainVisibleRenderY = tile->key.y;
    command.terrainVisibleSelectedPass = true;
    command.terrainVisibleSelectedTileIdentity = tile.get();
    command.terrainVisibleRenderTileIdentity = tile.get();
    command.terrainVisibleGridSize = 1;
    command.terrainVisibleMorph = 1.0f;
    command.terrainVisibleFade = 1.0f;
    const std::vector<RenderCommand> commands{command};

    RenderedTerrainSurfaceSampler sampler(submitted, *scheme, &commands);
    const auto [lng, lat] = pointIn(tile->bounds, 0.37, 0.61);
    terrain_edge::HeightSource expectedSource = terrain_edge::sourceOf(entry);
    ASSERT_TRUE(expectedSource.valid());
    expectedSource.gridSize = 1;
    expectedSource.morph = 1.0f;
    expectedSource.fade = 1.0f;
    expectedSource.webMercatorHeightV = true;
    const auto actual = sampler.sample(lng, lat);
    ASSERT_TRUE(actual.has_value());
    EXPECT_NEAR(terrain_edge::renderedHeight(expectedSource, lng, lat),
                *actual, 1e-5f);
    EXPECT_GT(*actual, 0.0f)
        << "z6 baked command must not inherit plan reliefFade=0";
}

TEST(RenderedTerrainSurfaceSamplerTest,
     PlannedEntryWithoutEmittedTerrainCommandIsNotSampleable) {
    auto scheme = TileScheme::createGeographicTMS();
    auto tile = makeTerrainTile(*scheme, TileKey{kScheme, 9, 160, 80});
    TileRenderEntry entry;
    entry.selectedKey = tile->key;
    entry.renderKey = tile->key;
    entry.selectedTile = tile.get();
    entry.renderTile = tile.get();
    const std::vector<TileRenderEntry> submitted{entry};
    const std::vector<RenderCommand> noCommands;
    RenderedTerrainSurfaceSampler sampler(submitted, *scheme, &noCommands);
    const auto [lng, lat] = pointIn(tile->bounds, 0.5, 0.25);
    EXPECT_FALSE(sampler.sample(lng, lat).has_value());
}

TEST(RenderedTerrainSurfaceSamplerTest,
     EmittedEdgeLutOverridesMorphAtTheGpuBoundary) {
    auto scheme = TileScheme::createGeographicTMS();
    auto tile = makeTerrainTile(*scheme, TileKey{kScheme, 9, 160, 80});
    tile->selectionFrameState.displacementGridSize = 2;
    tile->selectionFrameState.terrainMorphFactor = 0.0f;
    TileRenderEntry entry;
    entry.selectedKey = tile->key;
    entry.renderKey = tile->key;
    entry.selectedTile = tile.get();
    entry.renderTile = tile.get();
    const std::vector<TileRenderEntry> submitted{entry};

    TerrainEdgeLutTable lut;
    lut.gridSize = 2;
    lut.nodeCount[0] = 2;
    lut.delta[0][0] = 100.0f;
    lut.delta[0][1] = 200.0f;

    RenderCommand command;
    command.terrainRenderContent = true;
    command.terrainVisibleSurfaceValid = true;
    command.terrainVisibleSelectedZ = tile->key.z;
    command.terrainVisibleSelectedX = tile->key.x;
    command.terrainVisibleSelectedY = tile->key.y;
    command.terrainVisibleRenderZ = tile->key.z;
    command.terrainVisibleRenderX = tile->key.x;
    command.terrainVisibleRenderY = tile->key.y;
    command.terrainVisibleSelectedPass = true;
    command.terrainVisibleSelectedTileIdentity = tile.get();
    command.terrainVisibleRenderTileIdentity = tile.get();
    command.terrainVisibleGridSize = 2;
    command.terrainVisibleMorph = 0.0f;
    command.terrainVisibleFade = 1.0f;
    command.hasTerrainDisplacementFrame = true;
    command.gltfUniforms.heightDisplace = {0.0f, 80.0f, 1.0f, 2.0f};
    command.terrainVisibleEdgeSnapPacked = 4097.0f;
    command.terrainVisibleEdgeLutTable = &lut;
    const std::vector<RenderCommand> commands{command};

    RenderedTerrainSurfaceSampler sampler(submitted, *scheme, &commands);
    const auto [lng, lat] = pointIn(tile->bounds, 0.0, 0.5);
    const auto actual = sampler.sample(lng, lat);
    ASSERT_TRUE(actual.has_value());
    // W edge, grid=2, step=2, midway between LUT nodes. The own fine
    // endpoint samples are 0 and 60; deltas are 100 and 200.
    const auto gpuDelta = [](float delta) {
        return TerrainDisplacementTemplatePool::decodeEdgeLutDelta(
            TerrainDisplacementTemplatePool::encodeEdgeLutDelta(delta));
    };
    const float expected =
        (0.0f + gpuDelta(100.0f) + 60.0f + gpuDelta(200.0f)) * 0.5f;
    EXPECT_NEAR(*actual, expected, 2e-4f);
}

TEST(RenderedTerrainSurfaceSamplerTest,
     EdgeSnapWithoutUploadedLutMatchesShaderSelfTextureFallback) {
    auto scheme = TileScheme::createGeographicTMS();
    auto tile = makeTerrainTile(*scheme, TileKey{kScheme, 9, 160, 80});
    tile->selectionFrameState.displacementGridSize = 2;
    tile->selectionFrameState.terrainMorphFactor = 0.0f;
    TileRenderEntry entry;
    entry.selectedKey = tile->key;
    entry.renderKey = tile->key;
    entry.selectedTile = tile.get();
    entry.renderTile = tile.get();
    const std::vector<TileRenderEntry> submitted{entry};

    RenderCommand command;
    command.terrainRenderContent = true;
    command.terrainVisibleSurfaceValid = true;
    command.terrainVisibleSelectedZ = tile->key.z;
    command.terrainVisibleSelectedX = tile->key.x;
    command.terrainVisibleSelectedY = tile->key.y;
    command.terrainVisibleRenderZ = tile->key.z;
    command.terrainVisibleRenderX = tile->key.x;
    command.terrainVisibleRenderY = tile->key.y;
    command.terrainVisibleSelectedPass = true;
    command.terrainVisibleSelectedTileIdentity = tile.get();
    command.terrainVisibleRenderTileIdentity = tile.get();
    command.terrainVisibleGridSize = 2;
    command.terrainVisibleMorph = 0.0f;
    command.terrainVisibleFade = 1.0f;
    command.hasTerrainDisplacementFrame = true;
    command.gltfUniforms.heightDisplace = {0.0f, 80.0f, 1.0f, 2.0f};
    command.terrainVisibleEdgeSnapPacked = 1.0f;
    const std::vector<RenderCommand> commands{command};

    RenderedTerrainSurfaceSampler sampler(submitted, *scheme, &commands);
    const auto [lng, lat] = pointIn(tile->bounds, 0.0, 0.5);
    const auto actual = sampler.sample(lng, lat);
    ASSERT_TRUE(actual.has_value());
    EXPECT_NEAR(*actual, 30.0f, 2e-4f)
        << "edge snap overrides morph even when LUT upload fails";
}

TEST(RenderedTerrainSurfaceSamplerTest,
     BakedTerrainIgnoresPackedEdgeSnapUnsupportedByItsShader) {
    auto scheme = TileScheme::createGeographicTMS();
    auto tile = makeTerrainTile(*scheme, TileKey{kScheme, 9, 160, 80});
    tile->selectionFrameState.displacementGridSize = 2;
    tile->selectionFrameState.terrainMorphFactor = 0.25f;
    TileRenderEntry entry;
    entry.selectedKey = tile->key;
    entry.renderKey = tile->key;
    entry.selectedTile = tile.get();
    entry.renderTile = tile.get();
    const std::vector<TileRenderEntry> submitted{entry};

    RenderCommand command;
    command.terrainRenderContent = true;
    command.terrainVisibleSurfaceValid = true;
    command.terrainVisibleSelectedZ = tile->key.z;
    command.terrainVisibleSelectedX = tile->key.x;
    command.terrainVisibleSelectedY = tile->key.y;
    command.terrainVisibleRenderZ = tile->key.z;
    command.terrainVisibleRenderX = tile->key.x;
    command.terrainVisibleRenderY = tile->key.y;
    command.terrainVisibleSelectedPass = true;
    command.terrainVisibleSelectedTileIdentity = tile.get();
    command.terrainVisibleRenderTileIdentity = tile.get();
    command.terrainVisibleGridSize = 2;
    command.terrainVisibleMorph = 0.25f;
    command.terrainVisibleFade = 1.0f;
    command.hasTerrainDisplacementFrame = false;
    command.terrainVisibleEdgeSnapPacked = 1.0f;
    const std::vector<RenderCommand> commands{command};

    RenderedTerrainSurfaceSampler sampler(submitted, *scheme, &commands);
    // u=0.2 is inside the shader's W-edge epsilon band for grid=2, but the
    // baked surface still varies across the cell; stale snap would collapse
    // it to the W-edge node height.
    const auto [lng, lat] = pointIn(tile->bounds, 0.2, 0.5);
    const auto actual = sampler.sample(lng, lat);
    ASSERT_TRUE(actual.has_value());
    const auto source = terrain_edge::sourceOf(entry);
    auto expectedSource = source;
    expectedSource.webMercatorHeightV = true;
    const float expected = terrain_edge::renderedHeight(
        expectedSource, lng, lat);
    EXPECT_NEAR(*actual, expected, 1e-5f)
        << "gltfShader has no edge-snap branch, so the CPU sampler must "
           "preserve the baked command's emitted surface";
    EXPECT_GT(std::abs(*actual - 30.0f), 1.0f)
        << "the fixture must distinguish the baked surface from the stale "
           "W-edge snap result";
}

TEST(RenderedTerrainSurfaceSamplerTest,
     SameTileKeyFromAnotherTilesetCannotSatisfyMissingPrimaryCommand) {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey key{kScheme, 9, 160, 80};
    auto primary = makeTerrainTile(*scheme, key, 1.0f);
    auto additional = makeTerrainTile(*scheme, key, 5.0f);
    TileRenderEntry entry;
    entry.selectedKey = key;
    entry.renderKey = key;
    entry.selectedTile = primary.get();
    entry.renderTile = primary.get();

    RenderCommand command;
    command.terrainRenderContent = true;
    command.terrainVisibleSurfaceValid = true;
    command.terrainVisibleSelectedZ = key.z;
    command.terrainVisibleSelectedX = key.x;
    command.terrainVisibleSelectedY = key.y;
    command.terrainVisibleRenderZ = key.z;
    command.terrainVisibleRenderX = key.x;
    command.terrainVisibleRenderY = key.y;
    command.terrainVisibleSelectedPass = true;
    command.terrainVisibleSelectedTileIdentity = additional.get();
    command.terrainVisibleRenderTileIdentity = additional.get();
    const std::vector<RenderCommand> commands{command};

    RenderedTerrainSurfaceSampler sampler(
        std::vector<TileRenderEntry>{entry}, *scheme, &commands);
    const auto [lng, lat] = pointIn(primary->bounds, 0.5, 0.5);
    EXPECT_FALSE(sampler.sample(lng, lat).has_value());
}

TEST(RenderedTerrainSurfaceSamplerTest,
     CommandMatchingBuildsOneIndexAndPerformsOneLookupPerEntry) {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey keyA{kScheme, 9, 160, 80};
    const TileKey keyB{kScheme, 9, 161, 80};
    auto tileA = makeTerrainTile(*scheme, keyA);
    auto tileB = makeTerrainTile(*scheme, keyB);
    std::vector<TileRenderEntry> entries(2);
    entries[0].selectedKey = entries[0].renderKey = keyA;
    entries[0].selectedTile = entries[0].renderTile = tileA.get();
    entries[1].selectedKey = entries[1].renderKey = keyB;
    entries[1].selectedTile = entries[1].renderTile = tileB.get();

    std::vector<RenderCommand> commands(2);
    for (std::size_t i = 0; i < commands.size(); ++i) {
        auto& command = commands[i];
        const auto& entry = entries[i];
        command.terrainVisibleSurfaceValid = true;
        command.terrainVisibleSelectedZ = entry.selectedKey.z;
        command.terrainVisibleSelectedX = entry.selectedKey.x;
        command.terrainVisibleSelectedY = entry.selectedKey.y;
        command.terrainVisibleRenderZ = entry.renderKey.z;
        command.terrainVisibleRenderX = entry.renderKey.x;
        command.terrainVisibleRenderY = entry.renderKey.y;
        command.terrainVisibleSelectedPass = true;
        command.terrainVisibleSelectedTileIdentity = entry.selectedTile;
        command.terrainVisibleRenderTileIdentity = entry.renderTile;
        command.terrainVisibleGridSize = 2;
        command.terrainVisibleMorph = 1.0f;
        command.terrainVisibleFade = 1.0f;
    }

    RenderedTerrainSurfaceSampler sampler(entries, *scheme, &commands);
    EXPECT_EQ(2u, sampler.candidateCount());
    EXPECT_EQ(2u, sampler.stats().commandIndexEntries);
    EXPECT_EQ(2u, sampler.stats().commandLookups);
    EXPECT_EQ(0u, sampler.stats().lutCopyBytes);

    const auto areaSampler = sampler.makeAreaSampler(tileA->bounds);
    EXPECT_TRUE(areaSampler);
    EXPECT_EQ(1u, sampler.stats().areaSamplerBuilds);
    EXPECT_EQ(1u, sampler.stats().areaCandidateCopies);
}

TEST(RenderedTerrainSurfaceSamplerTest,
     CommandIndexKeepsCompositionFragmentsSharingTilePointersDistinct) {
    auto scheme = TileScheme::createGeographicTMS();
    auto source = makeTerrainTile(*scheme, TileKey{kScheme, 7, 40, 20});
    const TileKey childA{kScheme, 8, 80, 40};
    const TileKey childB{kScheme, 8, 81, 40};
    std::vector<TileRenderEntry> entries(2);
    entries[0].selectedKey = childA;
    entries[1].selectedKey = childB;
    for (auto& entry : entries) {
        entry.renderKey = source->key;
        entry.selectedTile = source.get();
        entry.renderTile = source.get();
    }
    std::vector<RenderCommand> commands(2);
    for (std::size_t i = 0; i < commands.size(); ++i) {
        auto& command = commands[i];
        command.terrainVisibleSurfaceValid = true;
        command.terrainVisibleSelectedZ = entries[i].selectedKey.z;
        command.terrainVisibleSelectedX = entries[i].selectedKey.x;
        command.terrainVisibleSelectedY = entries[i].selectedKey.y;
        command.terrainVisibleRenderZ = source->key.z;
        command.terrainVisibleRenderX = source->key.x;
        command.terrainVisibleRenderY = source->key.y;
        command.terrainVisibleSelectedPass = true;
        command.terrainVisibleSelectedTileIdentity = source.get();
        command.terrainVisibleRenderTileIdentity = source.get();
        command.terrainVisibleGridSize = i == 0 ? 1 : 2;
        command.terrainVisibleMorph = 1.0f;
        command.terrainVisibleFade = 1.0f;
    }

    RenderedTerrainSurfaceSampler sampler(entries, *scheme, &commands);
    EXPECT_EQ(2u, sampler.candidateCount());
    EXPECT_EQ(2u, sampler.stats().commandIndexEntries);
    const auto [lngA, latA] = pointIn(scheme->tileToRectangle(childA),
                                      0.37, 0.61);
    const auto [lngB, latB] = pointIn(scheme->tileToRectangle(childB),
                                      0.37, 0.61);
    const auto sampledA = sampler.sample(lngA, latA);
    const auto sampledB = sampler.sample(lngB, latB);
    ASSERT_TRUE(sampledA.has_value());
    ASSERT_TRUE(sampledB.has_value());
    EXPECT_NE(*sampledA, *sampledB)
        << "distinct fragment commands must retain their own grid contract";
}

TEST(RenderedTerrainSurfaceSamplerTest,
     RemapClipUsesTheFinalExpandedAncestorUvTransform) {
    auto scheme = TileScheme::createGeographicTMS();
    auto ancestor = makeTerrainTile(*scheme, TileKey{kScheme, 7, 40, 20});
    auto child = std::make_unique<TilesetTile>(
        TileKey{kScheme, 8, 80, 40},
        scheme->tileToRectangle(TileKey{kScheme, 8, 80, 40}));
    TileRenderEntry entry;
    entry.selectedKey = child->key;
    entry.renderKey = ancestor->key;
    entry.selectedTile = child.get();
    entry.renderTile = ancestor.get();
    entry.usesAncestorFallback = true;

    RenderCommand command;
    command.terrainRenderContent = true;
    command.terrainVisibleSurfaceValid = true;
    command.terrainVisibleSelectedZ = child->key.z;
    command.terrainVisibleSelectedX = child->key.x;
    command.terrainVisibleSelectedY = child->key.y;
    command.terrainVisibleRenderZ = ancestor->key.z;
    command.terrainVisibleRenderX = ancestor->key.x;
    command.terrainVisibleRenderY = ancestor->key.y;
    command.terrainVisibleSelectedPass = true;
    command.terrainVisibleSelectedTileIdentity = child.get();
    command.terrainVisibleRenderTileIdentity = ancestor.get();
    command.terrainVisibleGridSize = 2;
    command.terrainVisibleMorph = 1.0f;
    command.terrainVisibleFade = 1.0f;
    command.hasTerrainDisplacementFrame = true;
    command.gltfUniforms.heightDisplace = {0.0f, 80.0f, 1.0f, 2.0f};
    command.terrainVisibleClipMode = 2.0f;
    command.terrainVisibleClipUv = {-0.01f, -0.01f, 0.52f, 0.52f};
    const std::vector<RenderCommand> commands{command};

    RenderedTerrainSurfaceSampler sampler(
        std::vector<TileRenderEntry>{entry}, *scheme, &commands);
    const auto [lng, lat] = pointIn(child->bounds, 0.5, 0.5);
    const auto actual = sampler.sample(lng, lat);
    ASSERT_TRUE(actual.has_value());
    const double sourceU = -0.01 + 0.5 * 0.52;
    const double sourceV = -0.01 + 0.5 * 0.52;
    const double sampleLng =
        ancestor->bounds.west() + sourceU * ancestor->bounds.width();
    const double sampleLat =
        ancestor->bounds.north() - sourceV * ancestor->bounds.height();
    const float expected =
        DecodedHeightmapSampler::sampleHeightRenderGridQuantizedDecoded(
            *ancestor->content.renderContent.retainedHeightmap(),
            ancestor->bounds, sampleLng, sampleLat, 2,
            0.0f, 80.0f, 0.0f, 80.0f);
    EXPECT_NEAR(*actual, expected, 1e-5f);
}

TEST(RenderedTerrainSurfaceSamplerTest,
     DisplacementCommandUsesTheGpuRg16HeightQuantization) {
    auto scheme = TileScheme::createGeographicTMS();
    auto tile = makeTerrainTile(*scheme, TileKey{kScheme, 9, 160, 80});
    TileRenderEntry entry;
    entry.selectedKey = tile->key;
    entry.renderKey = tile->key;
    entry.selectedTile = tile.get();
    entry.renderTile = tile.get();

    RenderCommand command;
    command.terrainRenderContent = true;
    command.hasTerrainDisplacementFrame = true;
    command.terrainVisibleSurfaceValid = true;
    command.terrainVisibleSelectedZ = tile->key.z;
    command.terrainVisibleSelectedX = tile->key.x;
    command.terrainVisibleSelectedY = tile->key.y;
    command.terrainVisibleRenderZ = tile->key.z;
    command.terrainVisibleRenderX = tile->key.x;
    command.terrainVisibleRenderY = tile->key.y;
    command.terrainVisibleSelectedPass = true;
    command.terrainVisibleSelectedTileIdentity = tile.get();
    command.terrainVisibleRenderTileIdentity = tile.get();
    command.terrainVisibleGridSize = 2;
    command.terrainVisibleMorph = 1.0f;
    command.terrainVisibleFade = 1.0f;
    command.gltfUniforms.heightDisplace = {0.0f, 80.0f, 1.0f, 2.0f};
    const std::vector<RenderCommand> commands{command};

    RenderedTerrainSurfaceSampler sampler(
        std::vector<TileRenderEntry>{entry}, *scheme, &commands);
    const auto [lng, lat] = pointIn(tile->bounds, 0.37, 0.61);
    const auto actual = sampler.sample(lng, lat);
    ASSERT_TRUE(actual.has_value());
    const float expected =
        DecodedHeightmapSampler::sampleHeightRenderGridQuantized(
            *tile->content.renderContent.retainedHeightmap(), tile->bounds,
            lng, lat, 2, 0.0f, 80.0f);
    EXPECT_NEAR(*actual, expected, 1e-6f);
}

TEST(RenderedTerrainSurfaceSamplerTest,
     DisplacementCommandDecodesTheExactFadedGpuHeightRange) {
    auto scheme = TileScheme::createGeographicTMS();
    auto tile = makeTerrainTile(*scheme, TileKey{kScheme, 7, 40, 20});
    TileRenderEntry entry;
    entry.selectedKey = tile->key;
    entry.renderKey = tile->key;
    entry.selectedTile = tile.get();
    entry.renderTile = tile.get();

    constexpr float fade = 0.37f;
    RenderCommand command;
    command.terrainRenderContent = true;
    command.hasTerrainDisplacementFrame = true;
    command.terrainVisibleSurfaceValid = true;
    command.terrainVisibleSelectedZ = tile->key.z;
    command.terrainVisibleSelectedX = tile->key.x;
    command.terrainVisibleSelectedY = tile->key.y;
    command.terrainVisibleRenderZ = tile->key.z;
    command.terrainVisibleRenderX = tile->key.x;
    command.terrainVisibleRenderY = tile->key.y;
    command.terrainVisibleSelectedPass = true;
    command.terrainVisibleSelectedTileIdentity = tile.get();
    command.terrainVisibleRenderTileIdentity = tile.get();
    command.terrainVisibleGridSize = 2;
    command.terrainVisibleMorph = 1.0f;
    command.terrainVisibleFade = fade;
    command.gltfUniforms.heightDisplace =
        {0.0f, 80.0f * fade, 1.0f, 2.0f};
    const std::vector<RenderCommand> commands{command};

    RenderedTerrainSurfaceSampler sampler(
        std::vector<TileRenderEntry>{entry}, *scheme, &commands);
    const auto [lng, lat] = pointIn(tile->bounds, 0.37, 0.61);
    const auto actual = sampler.sample(lng, lat);
    ASSERT_TRUE(actual.has_value());
    const float expected =
        DecodedHeightmapSampler::sampleHeightRenderGridQuantizedDecoded(
            *tile->content.renderContent.retainedHeightmap(), tile->bounds,
            lng, lat, 2, 0.0f, 80.0f,
            command.gltfUniforms.heightDisplace[0],
            command.gltfUniforms.heightDisplace[1]);
    EXPECT_FLOAT_EQ(*actual, expected)
        << "the CPU contract must consume the same already-faded float32 mr "
           "pair as eeSampleTerrainHeight";
}

TEST(RenderedTerrainSurfaceSamplerTest,
     BakedCommandSamplesTheIndexedTriangleInsteadOfABilinearPatch) {
    auto scheme = TileScheme::createGeographicTMS();
    auto tile = makeTerrainTile(*scheme, TileKey{kScheme, 6, 20, 10});
    tile->content.renderContent.setRetainedHeightmap(
        makeNonPlanarCellHeightmap());
    TileRenderEntry entry;
    entry.selectedKey = tile->key;
    entry.renderKey = tile->key;
    entry.selectedTile = tile.get();
    entry.renderTile = tile.get();

    RenderCommand command;
    command.terrainRenderContent = true;
    command.terrainVisibleSurfaceValid = true;
    command.terrainVisibleSelectedZ = tile->key.z;
    command.terrainVisibleSelectedX = tile->key.x;
    command.terrainVisibleSelectedY = tile->key.y;
    command.terrainVisibleRenderZ = tile->key.z;
    command.terrainVisibleRenderX = tile->key.x;
    command.terrainVisibleRenderY = tile->key.y;
    command.terrainVisibleSelectedPass = true;
    command.terrainVisibleSelectedTileIdentity = tile.get();
    command.terrainVisibleRenderTileIdentity = tile.get();
    command.terrainVisibleGridSize = 1;
    command.terrainVisibleMorph = 1.0f;
    command.terrainVisibleFade = 1.0f;

    const std::vector<RenderCommand> commands{command};
    RenderedTerrainSurfaceSampler sampler(
        std::vector<TileRenderEntry>{entry}, *scheme, &commands);
    const auto [lng, lat] = pointIn(tile->bounds, 0.5, 0.5);
    const auto actual = sampler.sample(lng, lat);
    ASSERT_TRUE(actual.has_value());
    EXPECT_NEAR(0.0f, *actual, 1e-6f);
    EXPECT_NE(25.0f, *actual)
        << "the CPU clamp contract must follow the emitted b-c diagonal";
}

TEST(RenderedTerrainSurfaceSamplerTest,
     DisplacementCommandSamplesTheQuantizedIndexedTriangle) {
    auto scheme = TileScheme::createGeographicTMS();
    auto tile = makeTerrainTile(*scheme, TileKey{kScheme, 9, 160, 80});
    tile->content.renderContent.setRetainedHeightmap(
        makeNonPlanarCellHeightmap());
    TileRenderEntry entry;
    entry.selectedKey = tile->key;
    entry.renderKey = tile->key;
    entry.selectedTile = tile.get();
    entry.renderTile = tile.get();

    RenderCommand command;
    command.terrainRenderContent = true;
    command.hasTerrainDisplacementFrame = true;
    command.terrainVisibleSurfaceValid = true;
    command.terrainVisibleSelectedZ = tile->key.z;
    command.terrainVisibleSelectedX = tile->key.x;
    command.terrainVisibleSelectedY = tile->key.y;
    command.terrainVisibleRenderZ = tile->key.z;
    command.terrainVisibleRenderX = tile->key.x;
    command.terrainVisibleRenderY = tile->key.y;
    command.terrainVisibleSelectedPass = true;
    command.terrainVisibleSelectedTileIdentity = tile.get();
    command.terrainVisibleRenderTileIdentity = tile.get();
    command.terrainVisibleGridSize = 1;
    command.terrainVisibleMorph = 1.0f;
    command.terrainVisibleFade = 1.0f;
    command.gltfUniforms.heightDisplace = {0.0f, 100.0f, 1.0f, 1.0f};

    const std::vector<RenderCommand> commands{command};
    RenderedTerrainSurfaceSampler sampler(
        std::vector<TileRenderEntry>{entry}, *scheme, &commands);
    const auto [lng, lat] = pointIn(tile->bounds, 0.5, 0.5);
    const auto actual = sampler.sample(lng, lat);
    ASSERT_TRUE(actual.has_value());
    EXPECT_FLOAT_EQ(0.0f, *actual);
    EXPECT_NE(25.0f, *actual)
        << "RG16 node decoding must still use the template's triangle plane";
}

TEST(RenderedTerrainSurfaceSamplerTest,
     BakedCommandConstructsGeomorphAtFineVerticesBeforeRasterization) {
    auto scheme = TileScheme::createGeographicTMS();
    auto tile = makeTerrainTile(*scheme, TileKey{kScheme, 6, 20, 10});
    tile->content.renderContent.setRetainedHeightmap(
        makeNonPlanarGeomorphHeightmap());
    TileRenderEntry entry;
    entry.selectedKey = tile->key;
    entry.renderKey = tile->key;
    entry.selectedTile = tile.get();
    entry.renderTile = tile.get();

    RenderCommand command;
    command.terrainRenderContent = true;
    command.terrainVisibleSurfaceValid = true;
    command.terrainVisibleSelectedZ = tile->key.z;
    command.terrainVisibleSelectedX = tile->key.x;
    command.terrainVisibleSelectedY = tile->key.y;
    command.terrainVisibleRenderZ = tile->key.z;
    command.terrainVisibleRenderX = tile->key.x;
    command.terrainVisibleRenderY = tile->key.y;
    command.terrainVisibleSelectedPass = true;
    command.terrainVisibleSelectedTileIdentity = tile.get();
    command.terrainVisibleRenderTileIdentity = tile.get();
    command.terrainVisibleGridSize = 2;
    command.terrainVisibleMorph = 0.5f;
    command.terrainVisibleFade = 1.0f;

    const std::vector<RenderCommand> commands{command};
    RenderedTerrainSurfaceSampler sampler(
        std::vector<TileRenderEntry>{entry}, *scheme, &commands);
    const auto [lng, lat] = pointIn(tile->bounds, 0.5, 0.5);
    const auto actual = sampler.sample(lng, lat);
    ASSERT_TRUE(actual.has_value());
    auto expectedSource = terrain_edge::sourceOf(entry);
    expectedSource.gridSize = 2;
    expectedSource.morph = 0.5f;
    expectedSource.fade = 1.0f;
    expectedSource.webMercatorHeightV = true;
    EXPECT_NEAR(terrain_edge::renderedHeight(expectedSource, lng, lat),
                *actual, 1e-5f);
    EXPECT_GT(*actual, 10.0f)
        << "sampling a separate half-density triangle loses coarse bilinear";
}

TEST(RenderedTerrainSurfaceSamplerTest,
     DisplacementCommandConstructsQuantizedGeomorphAtFineVertices) {
    auto scheme = TileScheme::createGeographicTMS();
    auto tile = makeTerrainTile(*scheme, TileKey{kScheme, 9, 160, 80});
    tile->content.renderContent.setRetainedHeightmap(
        makeNonPlanarGeomorphHeightmap());
    TileRenderEntry entry;
    entry.selectedKey = tile->key;
    entry.renderKey = tile->key;
    entry.selectedTile = tile.get();
    entry.renderTile = tile.get();

    RenderCommand command;
    command.terrainRenderContent = true;
    command.hasTerrainDisplacementFrame = true;
    command.terrainVisibleSurfaceValid = true;
    command.terrainVisibleSelectedZ = tile->key.z;
    command.terrainVisibleSelectedX = tile->key.x;
    command.terrainVisibleSelectedY = tile->key.y;
    command.terrainVisibleRenderZ = tile->key.z;
    command.terrainVisibleRenderX = tile->key.x;
    command.terrainVisibleRenderY = tile->key.y;
    command.terrainVisibleSelectedPass = true;
    command.terrainVisibleSelectedTileIdentity = tile.get();
    command.terrainVisibleRenderTileIdentity = tile.get();
    command.terrainVisibleGridSize = 2;
    command.terrainVisibleMorph = 0.5f;
    command.terrainVisibleFade = 1.0f;
    command.gltfUniforms.heightDisplace = {0.0f, 100.0f, 1.0f, 2.0f};

    const std::vector<RenderCommand> commands{command};
    RenderedTerrainSurfaceSampler sampler(
        std::vector<TileRenderEntry>{entry}, *scheme, &commands);
    const auto [lng, lat] = pointIn(tile->bounds, 0.5, 0.5);
    const auto actual = sampler.sample(lng, lat);
    ASSERT_TRUE(actual.has_value());
    EXPECT_NEAR(12.5f, *actual, 2e-3f);
}

TEST(RenderedTerrainSurfaceSamplerTest,
     BakedWebMercatorCommandUsesProjectedDemRowCoordinate) {
    auto scheme = TileScheme::createXYZWebMercator();
    const TileKey key{"XYZ-WebMercator", 3, 4, 1};
    auto tile = makeTerrainTile(*scheme, key);
    tile->content.renderContent.setRetainedHeightmap(
        makeNorthSouthRampHeightmap());
    TileRenderEntry entry;
    entry.selectedKey = key;
    entry.renderKey = key;
    entry.selectedTile = tile.get();
    entry.renderTile = tile.get();

    RenderCommand command;
    command.terrainRenderContent = true;
    command.terrainVisibleSurfaceValid = true;
    command.terrainVisibleSelectedZ = key.z;
    command.terrainVisibleSelectedX = key.x;
    command.terrainVisibleSelectedY = key.y;
    command.terrainVisibleRenderZ = key.z;
    command.terrainVisibleRenderX = key.x;
    command.terrainVisibleRenderY = key.y;
    command.terrainVisibleSelectedPass = true;
    command.terrainVisibleSelectedTileIdentity = tile.get();
    command.terrainVisibleRenderTileIdentity = tile.get();
    command.terrainVisibleGridSize = 2;
    command.terrainVisibleMorph = 1.0f;
    command.terrainVisibleFade = 1.0f;

    const std::vector<RenderCommand> commands{command};
    RenderedTerrainSurfaceSampler sampler(
        std::vector<TileRenderEntry>{entry}, *scheme, &commands);
    const auto [lng, lat] = pointIn(tile->bounds, 0.5, 0.25);
    const auto actual = sampler.sample(lng, lat);
    ASSERT_TRUE(actual.has_value());
    const auto mercatorY = [](double latitude) {
        return std::log(std::tan(M_PI * 0.25 + latitude * 0.5));
    };
    const double middleNodeLatitude =
        tile->bounds.north() - 0.5 * tile->bounds.height();
    const double middleNodeProjectedV =
        (mercatorY(tile->bounds.north()) - mercatorY(middleNodeLatitude)) /
        (mercatorY(tile->bounds.north()) -
         mercatorY(tile->bounds.south()));
    // Query v=.25 lies halfway between baked geometry rows 0 and 1. The
    // rasterized height is therefore halfway between DEM row 0 and the
    // Mercator-projected sample owned by geometry row 1.
    const double expected = 50.0 * middleNodeProjectedV;
    EXPECT_NEAR(expected, *actual, 1e-4);
    const double continuouslyProjectedQuery =
        100.0 * (mercatorY(tile->bounds.north()) - mercatorY(lat)) /
        (mercatorY(tile->bounds.north()) - mercatorY(tile->bounds.south()));
    EXPECT_GT(std::abs(*actual - continuouslyProjectedQuery), 1.0)
        << "query projection before cell selection is not the baked mesh";
}
