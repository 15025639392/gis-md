#include <gtest/gtest.h>

#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"

#include <memory>

using namespace earth_engine;

namespace earth_engine {
struct TilesetTestAccess {
    static TilesetTile* ensureTile(Tileset& tileset, const TileKey& key) {
        return tileset.contentAccess_.ensureTile(key);
    }

    // Height comes from a retained DecodedHeightmap (the GPU-displacement
    // source of truth); mirrors how real heightmap terrain tiles are delivered.
    static void setLoadedHeightmapTerrainContent(
        TilesetTile& tile,
        std::unique_ptr<DecodedHeightmap> heightmap) {
        tile.content.renderContent.setTerrainRenderContent(true);
        tile.content.renderContent.setRetainedHeightmap(std::move(heightmap));
    }
};
} // namespace earth_engine

namespace {

std::unique_ptr<DecodedHeightmap> makeFlatHeightmap(float heightMeters) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->stagedHeights = {
        heightMeters,
        heightMeters,
        heightMeters,
        heightMeters};
    heightmap->assignHeights();
    return heightmap;
}

// Row-major, north→south rows / west→east cols: {NW, NE, SW, SE}.
std::unique_ptr<DecodedHeightmap> makeCornerHeightmap(
    float nw, float ne, float sw, float se) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->stagedHeights = {nw, ne, sw, se};
    heightmap->assignHeights();
    heightmap->minHeight = std::min({nw, ne, sw, se});
    heightmap->maxHeight = std::max({nw, ne, sw, se});
    return heightmap;
}

class ContentTerrainQuadtreeProvider final : public TilesetContentProvider {
public:
    std::string id() const override { return "content-terrain"; }
    bool supportsTile(const TileKey&) const override { return false; }
    bool providesTerrainQuadtree() const override { return true; }
    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callback(key, TileContentLoadResult::retryLater());
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }
};

Tileset makeContentTerrainSamplingTileset() {
    return Tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::make_unique<ContentTerrainQuadtreeProvider>());
}

} // namespace

TEST(TilesetSampleHeightTest, ContentTerrainQuadtreeSamplesLoadedTerrain) {
    Tileset tileset = makeContentTerrainSamplingTileset();
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    // Corner heights (NW=30, NE=40, SW=10, SE=20) bilinearly interpolate to
    // 17.5 at (0.25 from west, 0.25 from south).
    TilesetTestAccess::setLoadedHeightmapTerrainContent(
        *root,
        makeCornerHeightmap(30.0f, 40.0f, 10.0f, 20.0f));

    const Rectangle bounds = tileset.tileScheme().tileToRectangle(rootKey);
    const double longitude = bounds.west() + bounds.width() * 0.25;
    const double latitude = bounds.south() + bounds.height() * 0.25;

    EXPECT_NEAR(tileset.sampleHeight(longitude, latitude), 17.5f, 1e-4f);
}

TEST(TilesetSampleHeightTest,
     ContentTerrainQuadtreeUsesMostDetailedLoadedTerrainTile) {
    Tileset tileset = makeContentTerrainSamplingTileset();
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    TilesetTile* child = TilesetTestAccess::ensureTile(tileset, childKey);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(child, nullptr);

    TilesetTestAccess::setLoadedHeightmapTerrainContent(
        *root, makeFlatHeightmap(10.0f));
    TilesetTestAccess::setLoadedHeightmapTerrainContent(
        *child, makeFlatHeightmap(42.0f));

    const Rectangle childBounds =
        tileset.tileScheme().tileToRectangle(childKey);
    const double longitude = childBounds.west() + childBounds.width() * 0.25;
    const double latitude = childBounds.south() + childBounds.height() * 0.25;

    EXPECT_NEAR(tileset.sampleHeight(longitude, latitude), 42.0f, 1e-4f);
}

TEST(TilesetSampleHeightTest,
     ContentTerrainQuadtreeFallsBackToLoadedAncestorTerrain) {
    Tileset tileset = makeContentTerrainSamplingTileset();
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    // Child exists but has no terrain data → scan falls back to the covering
    // ancestor that has a retained heightmap.
    ASSERT_NE(TilesetTestAccess::ensureTile(tileset, childKey), nullptr);

    TilesetTestAccess::setLoadedHeightmapTerrainContent(
        *root, makeFlatHeightmap(123.0f));

    const Rectangle childBounds =
        tileset.tileScheme().tileToRectangle(childKey);
    const double longitude = childBounds.west() + childBounds.width() * 0.25;
    const double latitude = childBounds.south() + childBounds.height() * 0.25;

    EXPECT_NEAR(tileset.sampleHeight(longitude, latitude), 123.0f, 1e-4f);
}
