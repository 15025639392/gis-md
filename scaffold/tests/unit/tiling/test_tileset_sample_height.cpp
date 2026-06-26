#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"

#include <memory>

using namespace earth_engine;

namespace earth_engine {
struct TilesetTestAccess {
    static Tileset makeLegacyTerrainTileset(
        std::unique_ptr<TerrainProvider> terrainProvider,
        std::unique_ptr<TileScheme> tileScheme) {
        return Tileset(
            TilesetTerrainProviders(nullptr),
            std::move(tileScheme),
            {},
            nullptr,
            TilesetOptions{});
    }

    static TilesetTile* ensureTile(Tileset& tileset, const TileKey& key) {
        return tileset.contentAccess_.ensureTile(key);
    }

    static void putTerrainCache(
        Tileset& tileset,
        const TileKey& key,
        std::unique_ptr<DecodedHeightmap> heightmap) {
        tileset.contentLifecycle_.legacyHeightmapTerrainCache()[TileCacheKey::forTile(key)] =
            std::move(heightmap);
    }

    static void setLoadedGltfTerrainContent(
        TilesetTile& tile,
        std::unique_ptr<GltfModel> model) {
        tile.content.renderContent.prepareGltfContent(
            std::move(model),
            Mat4::identity());
        tile.content.renderContent.setTerrainRenderContent(true);
        tile.markRenderContentDone();
    }
};
} // namespace earth_engine

namespace {

std::unique_ptr<DecodedHeightmap> makeFlatHeightmap(float heightMeters) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights = {
        heightMeters,
        heightMeters,
        heightMeters,
        heightMeters};
    heightmap->minHeight = heightMeters;
    heightmap->maxHeight = heightMeters;
    return heightmap;
}

std::unique_ptr<GltfModel> makeTerrainGltfTriangle(
    const Rectangle& bounds,
    double southwestHeight,
    double southeastHeight,
    double northwestHeight) {
    auto model = std::make_unique<GltfModel>();
    GltfPrimitive primitive;
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    primitive.vertices.resize(3);
    primitive.vertices[0].positionEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(
            bounds.west(),
            bounds.south(),
            southwestHeight));
    primitive.vertices[1].positionEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(
            bounds.east(),
            bounds.south(),
            southeastHeight));
    primitive.vertices[2].positionEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(
            bounds.west(),
            bounds.north(),
            northwestHeight));
    primitive.indices = {0, 1, 2};
    primitive.primitiveMode = GltfPrimitiveMode::Triangles;
    primitive.runtime.nodeIndex = 0;
    primitive.runtime.baseVertices = primitive.vertices;
    model->primitives.push_back(std::move(primitive));
    model->rasterOverlayDetails.setGeographicRectangle(bounds);
    return model;
}

std::unique_ptr<GltfModel> makeStackedTerrainGltfTriangles(
    const Rectangle& bounds,
    double lowerHeight,
    double upperHeight) {
    std::unique_ptr<GltfModel> model =
        makeTerrainGltfTriangle(
            bounds,
            lowerHeight,
            lowerHeight,
            lowerHeight);
    std::unique_ptr<GltfModel> upper =
        makeTerrainGltfTriangle(
            bounds,
            upperHeight,
            upperHeight,
            upperHeight);
    model->primitives.push_back(std::move(upper->primitives.front()));
    return model;
}

std::pair<double, double> tileCenter(
    const TileScheme& scheme,
    const TileKey& key) {
    const Rectangle bounds = scheme.tileToRectangle(key);
    return {
        (bounds.west() + bounds.east()) * 0.5,
        (bounds.south() + bounds.north()) * 0.5};
}

class TestLegacyTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "test-legacy-terrain"; }
    std::string schemeId() const override { return "Geographic-TMS"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 24; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override { return {}; }
    void requestTile(
        const TileKey& key,
        CancellationToken,
        TerrainCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callback(key, TerrainTileLoadResult::retryLater());
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*,
        size_t) override {
        return {};
    }
};

Tileset makeHeightSamplingTileset() {
    return TilesetTestAccess::makeLegacyTerrainTileset(
        std::make_unique<TestLegacyTerrainProvider>(),
        TileScheme::createGeographicTMS());
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

TEST(TilesetSampleHeightTest, UsesMostDetailedLoadedTerrainTile) {
    Tileset tileset = makeHeightSamplingTileset();
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};

    TilesetTestAccess::ensureTile(tileset, rootKey);
    TilesetTestAccess::ensureTile(tileset, childKey);
    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(10.0f));
    TilesetTestAccess::putTerrainCache(
        tileset,
        childKey,
        makeFlatHeightmap(42.0f));

    const auto [longitude, latitude] =
        tileCenter(tileset.tileScheme(), childKey);

    EXPECT_NEAR(tileset.sampleHeight(longitude, latitude), 42.0f, 1e-6f);
}

TEST(TilesetSampleHeightTest, FallsBackToLoadedAncestorTerrain) {
    Tileset tileset = makeHeightSamplingTileset();
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};

    TilesetTestAccess::ensureTile(tileset, rootKey);
    TilesetTestAccess::ensureTile(tileset, childKey);
    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(123.0f));

    const auto [longitude, latitude] =
        tileCenter(tileset.tileScheme(), childKey);

    EXPECT_NEAR(tileset.sampleHeight(longitude, latitude), 123.0f, 1e-6f);
}

TEST(TilesetSampleHeightTest,
     ContentTerrainQuadtreeIgnoresLegacyHeightmapTerrainCache) {
    Tileset tileset = makeContentTerrainSamplingTileset();
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};

    TilesetTestAccess::ensureTile(tileset, rootKey);
    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(321.0f));

    const auto [longitude, latitude] =
        tileCenter(tileset.tileScheme(), rootKey);

    EXPECT_NEAR(tileset.sampleHeight(longitude, latitude), 0.0f, 1e-6f);
}

TEST(TilesetSampleHeightTest, ContentTerrainQuadtreeSamplesLoadedGltfTerrain) {
    Tileset tileset = makeContentTerrainSamplingTileset();
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(321.0f));
    const Rectangle bounds = tileset.tileScheme().tileToRectangle(rootKey);
    TilesetTestAccess::setLoadedGltfTerrainContent(
        *root,
        makeTerrainGltfTriangle(bounds, 10.0, 20.0, 30.0));

    const double longitude = bounds.west() + bounds.width() * 0.25;
    const double latitude = bounds.south() + bounds.height() * 0.25;

    EXPECT_NEAR(tileset.sampleHeight(longitude, latitude), 17.5f, 1e-4f);
}

TEST(TilesetSampleHeightTest,
     ContentTerrainQuadtreeUsesMostDetailedLoadedGltfTerrainTile) {
    Tileset tileset = makeContentTerrainSamplingTileset();
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    TilesetTile* child = TilesetTestAccess::ensureTile(tileset, childKey);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(child, nullptr);

    const Rectangle rootBounds = tileset.tileScheme().tileToRectangle(rootKey);
    const Rectangle childBounds =
        tileset.tileScheme().tileToRectangle(childKey);
    TilesetTestAccess::setLoadedGltfTerrainContent(
        *root,
        makeTerrainGltfTriangle(rootBounds, 10.0, 10.0, 10.0));
    TilesetTestAccess::setLoadedGltfTerrainContent(
        *child,
        makeTerrainGltfTriangle(childBounds, 42.0, 42.0, 42.0));

    const double longitude = childBounds.west() + childBounds.width() * 0.25;
    const double latitude = childBounds.south() + childBounds.height() * 0.25;

    EXPECT_NEAR(tileset.sampleHeight(longitude, latitude), 42.0f, 1e-4f);
}

TEST(TilesetSampleHeightTest,
     ContentTerrainQuadtreeFallsBackToLoadedGltfAncestorTerrain) {
    Tileset tileset = makeContentTerrainSamplingTileset();
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(TilesetTestAccess::ensureTile(tileset, childKey), nullptr);

    const Rectangle rootBounds = tileset.tileScheme().tileToRectangle(rootKey);
    const Rectangle childBounds =
        tileset.tileScheme().tileToRectangle(childKey);
    TilesetTestAccess::setLoadedGltfTerrainContent(
        *root,
        makeTerrainGltfTriangle(rootBounds, 123.0, 123.0, 123.0));

    const double longitude = childBounds.west() + childBounds.width() * 0.25;
    const double latitude = childBounds.south() + childBounds.height() * 0.25;

    EXPECT_NEAR(tileset.sampleHeight(longitude, latitude), 123.0f, 1e-4f);
}

TEST(TilesetSampleHeightTest,
     ContentTerrainQuadtreeReturnsHighestLoadedGltfTerrainAtLocationLikeCesiumNative) {
    Tileset tileset = makeContentTerrainSamplingTileset();
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);

    const Rectangle rootBounds = tileset.tileScheme().tileToRectangle(rootKey);
    TilesetTestAccess::setLoadedGltfTerrainContent(
        *root,
        makeStackedTerrainGltfTriangles(rootBounds, 78.0, 83.0));

    const double longitude = rootBounds.west() + rootBounds.width() * 0.25;
    const double latitude = rootBounds.south() + rootBounds.height() * 0.25;

    EXPECT_NEAR(tileset.sampleHeight(longitude, latitude), 83.0f, 1e-4f);
}
