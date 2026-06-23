#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/LoadedTerrainHeightSampler.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TilesetTile.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace earth_engine;

namespace {

std::string cacheKeyFor(const TileKey& key) {
    return key.schemeId + ":" +
           std::to_string(key.z) + ":" +
           std::to_string(key.x) + ":" +
           std::to_string(key.y);
}

std::unique_ptr<DecodedHeightmap> makeFlatHeightmap(float height) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights.assign(4, height);
    heightmap->minHeight = height;
    heightmap->maxHeight = height;
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

void appendTerrainGltfTriangle(GltfModel& model,
                               const Rectangle& bounds,
                               double southwestHeight,
                               double southeastHeight,
                               double northwestHeight) {
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
    primitive.runtime.nodeIndex =
        static_cast<int32_t>(model.primitives.size());
    primitive.runtime.baseVertices = primitive.vertices;
    model.primitives.push_back(std::move(primitive));
    model.rasterOverlayDetails.setGeographicRectangle(bounds);
}

std::unique_ptr<GltfModel> makeStackedTerrainGltfTriangles(
    const Rectangle& bounds,
    double lowerHeight,
    double upperHeight) {
    auto model = std::make_unique<GltfModel>();
    appendTerrainGltfTriangle(
        *model,
        bounds,
        lowerHeight,
        lowerHeight,
        lowerHeight);
    appendTerrainGltfTriangle(
        *model,
        bounds,
        upperHeight,
        upperHeight,
        upperHeight);
    return model;
}

std::unique_ptr<GltfModel> makeFlatTerrainGltfQuad(
    const Rectangle& bounds,
    double height,
    GltfPrimitiveMode primitiveMode,
    std::vector<uint32_t> indices) {
    auto model = std::make_unique<GltfModel>();
    GltfPrimitive primitive;
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    primitive.vertices.resize(4);
    primitive.vertices[0].positionEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(bounds.west(), bounds.south(), height));
    primitive.vertices[1].positionEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(bounds.east(), bounds.south(), height));
    primitive.vertices[2].positionEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(bounds.west(), bounds.north(), height));
    primitive.vertices[3].positionEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(bounds.east(), bounds.north(), height));
    primitive.indices = std::move(indices);
    primitive.primitiveMode = primitiveMode;
    primitive.runtime.nodeIndex = 0;
    primitive.runtime.baseVertices = primitive.vertices;
    model->primitives.push_back(std::move(primitive));
    model->rasterOverlayDetails.setGeographicRectangle(bounds);
    return model;
}

std::unique_ptr<GltfModel> makeTransformedTerrainGltfTriangle(
    const Rectangle& bounds,
    double southwestHeight,
    double southeastHeight,
    double northwestHeight,
    const Mat4& transform) {
    std::unique_ptr<GltfModel> model = makeTerrainGltfTriangle(
        bounds,
        southwestHeight,
        southeastHeight,
        northwestHeight);
    for (GltfPrimitive& primitive : model->primitives) {
        for (SurfaceVertex& vertex : primitive.vertices) {
            vertex.positionEcef = transform * vertex.positionEcef;
        }
        primitive.runtime.baseVertices = primitive.vertices;
    }
    return model;
}

void putTile(
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    const TileKey& key,
    const Rectangle& bounds) {
    tiles.emplace(cacheKeyFor(key), std::make_unique<TilesetTile>(key, bounds));
}

void putLoadedGltfTerrainTile(
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    const TileKey& key,
    const Rectangle& bounds,
    std::unique_ptr<GltfModel> model,
    const Mat4& contentTransform = Mat4::identity()) {
    auto tile = std::make_unique<TilesetTile>(key, bounds);
    tile->content.renderContent.prepareGltfContent(
        std::move(model),
        contentTransform);
    tile->content.renderContent.setTerrainRenderContent(true);
    tile->markRenderContentDone();
    tiles.emplace(cacheKeyFor(key), std::move(tile));
}

} // namespace

TEST(LoadedTerrainHeightSamplerTest, UsesBestLoadedTerrainTile) {
    auto scheme = TileScheme::createGeographicTMS();
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};
    putTile(tiles, rootKey, scheme->tileToRectangle(rootKey));
    putTile(tiles, childKey, scheme->tileToRectangle(childKey));
    terrainCache.emplace(cacheKeyFor(rootKey), makeFlatHeightmap(10.0f));
    terrainCache.emplace(cacheKeyFor(childKey), makeFlatHeightmap(42.0f));

    const Rectangle childBounds = scheme->tileToRectangle(childKey);
    const double longitude = (childBounds.west() + childBounds.east()) * 0.5;
    const double latitude = (childBounds.south() + childBounds.north()) * 0.5;

    EXPECT_NEAR(
        42.0f,
        LoadedTerrainHeightSampler::sampleHeight(
            tiles,
            terrainCache,
            longitude,
            latitude,
            LoadedTerrainHeightCacheMode::IncludeLegacyHeightmap),
        1e-6f);
}

TEST(LoadedTerrainHeightSamplerTest, FallsBackToLoadedAncestorTerrain) {
    auto scheme = TileScheme::createGeographicTMS();
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};
    putTile(tiles, rootKey, scheme->tileToRectangle(rootKey));
    putTile(tiles, childKey, scheme->tileToRectangle(childKey));
    terrainCache.emplace(cacheKeyFor(rootKey), makeFlatHeightmap(123.0f));

    const Rectangle childBounds = scheme->tileToRectangle(childKey);
    const double longitude = (childBounds.west() + childBounds.east()) * 0.5;
    const double latitude = (childBounds.south() + childBounds.north()) * 0.5;

    EXPECT_NEAR(
        123.0f,
        LoadedTerrainHeightSampler::sampleHeight(
            tiles,
            terrainCache,
            longitude,
            latitude,
            LoadedTerrainHeightCacheMode::IncludeLegacyHeightmap),
        1e-6f);
}

TEST(LoadedTerrainHeightSamplerTest, SamplesLoadedGltfTerrainTile) {
    auto scheme = TileScheme::createGeographicTMS();
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const Rectangle bounds = scheme->tileToRectangle(rootKey);
    putLoadedGltfTerrainTile(
        tiles,
        rootKey,
        bounds,
        makeTerrainGltfTriangle(bounds, 10.0, 20.0, 30.0));

    const double longitude = bounds.west() + bounds.width() * 0.25;
    const double latitude = bounds.south() + bounds.height() * 0.25;

    EXPECT_NEAR(
        17.5f,
        LoadedTerrainHeightSampler::sampleHeight(
            tiles,
            terrainCache,
            longitude,
            latitude,
            LoadedTerrainHeightCacheMode::ContentOwnedTerrainOnly),
        1e-4f);
}

TEST(LoadedTerrainHeightSamplerTest,
     ContentOwnedTerrainModeIgnoresLegacyHeightmapCache) {
    auto scheme = TileScheme::createGeographicTMS();
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const Rectangle bounds = scheme->tileToRectangle(rootKey);
    putLoadedGltfTerrainTile(
        tiles,
        rootKey,
        bounds,
        makeTerrainGltfTriangle(bounds, 10.0, 20.0, 30.0));
    terrainCache.emplace(cacheKeyFor(rootKey), makeFlatHeightmap(321.0f));

    const double longitude = bounds.west() + bounds.width() * 0.25;
    const double latitude = bounds.south() + bounds.height() * 0.25;

    EXPECT_NEAR(
        17.5f,
        LoadedTerrainHeightSampler::sampleHeight(
            tiles,
            terrainCache,
            longitude,
            latitude,
            LoadedTerrainHeightCacheMode::ContentOwnedTerrainOnly),
        1e-4f);
}

TEST(LoadedTerrainHeightSamplerTest,
     SamplesHighestGltfTerrainSurfaceWithinTileLikeCesiumNative) {
    auto scheme = TileScheme::createGeographicTMS();
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const Rectangle bounds = scheme->tileToRectangle(rootKey);
    putLoadedGltfTerrainTile(
        tiles,
        rootKey,
        bounds,
        makeStackedTerrainGltfTriangles(bounds, 78.0, 83.0));

    const double longitude = bounds.west() + bounds.width() * 0.25;
    const double latitude = bounds.south() + bounds.height() * 0.25;

    EXPECT_NEAR(
        83.0f,
        LoadedTerrainHeightSampler::sampleHeight(
            tiles,
            terrainCache,
            longitude,
            latitude,
            LoadedTerrainHeightCacheMode::ContentOwnedTerrainOnly),
        1e-4f);
}

TEST(LoadedTerrainHeightSamplerTest,
     SamplesHighestGltfTerrainSurfaceAcrossEqualDetailTilesLikeCesiumNative) {
    auto scheme = TileScheme::createGeographicTMS();
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;

    const TileKey lowerKey{"Geographic-TMS", 1, 0, 0};
    const TileKey upperKey{"Geographic-TMS", 1, 1, 0};
    const Rectangle bounds = scheme->tileToRectangle(lowerKey);
    putLoadedGltfTerrainTile(
        tiles,
        lowerKey,
        bounds,
        makeTerrainGltfTriangle(bounds, 78.0, 78.0, 78.0));
    putLoadedGltfTerrainTile(
        tiles,
        upperKey,
        bounds,
        makeTerrainGltfTriangle(bounds, 83.0, 83.0, 83.0));

    const double longitude = bounds.west() + bounds.width() * 0.25;
    const double latitude = bounds.south() + bounds.height() * 0.25;

    EXPECT_NEAR(
        83.0f,
        LoadedTerrainHeightSampler::sampleHeight(
            tiles,
            terrainCache,
            longitude,
            latitude,
            LoadedTerrainHeightCacheMode::ContentOwnedTerrainOnly),
        1e-4f);
}

TEST(LoadedTerrainHeightSamplerTest,
     SamplesLoadedGltfTerrainTriangleStripLikeCesiumNative) {
    auto scheme = TileScheme::createGeographicTMS();
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const Rectangle bounds = scheme->tileToRectangle(rootKey);
    putLoadedGltfTerrainTile(
        tiles,
        rootKey,
        bounds,
        makeFlatTerrainGltfQuad(
            bounds,
            42.0,
            GltfPrimitiveMode::TriangleStrip,
            {0, 1, 2, 3}));

    const double longitude = bounds.west() + bounds.width() * 0.25;
    const double latitude = bounds.south() + bounds.height() * 0.25;

    EXPECT_NEAR(
        42.0f,
        LoadedTerrainHeightSampler::sampleHeight(
            tiles,
            terrainCache,
            longitude,
            latitude,
            LoadedTerrainHeightCacheMode::ContentOwnedTerrainOnly),
        1e-4f);
}

TEST(LoadedTerrainHeightSamplerTest,
     SamplesLoadedGltfTerrainTriangleFanLikeCesiumNative) {
    auto scheme = TileScheme::createGeographicTMS();
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const Rectangle bounds = scheme->tileToRectangle(rootKey);
    putLoadedGltfTerrainTile(
        tiles,
        rootKey,
        bounds,
        makeFlatTerrainGltfQuad(
            bounds,
            24.0,
            GltfPrimitiveMode::TriangleFan,
            {0, 1, 3, 2}));

    const double longitude = bounds.west() + bounds.width() * 0.25;
    const double latitude = bounds.south() + bounds.height() * 0.25;

    EXPECT_NEAR(
        24.0f,
        LoadedTerrainHeightSampler::sampleHeight(
            tiles,
            terrainCache,
            longitude,
            latitude,
            LoadedTerrainHeightCacheMode::ContentOwnedTerrainOnly),
        1e-4f);
}

TEST(LoadedTerrainHeightSamplerTest,
     SamplesLoadedGltfTerrainWithContentTransform) {
    auto scheme = TileScheme::createGeographicTMS();
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>>
        transformedTiles;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> bakedTiles;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const Rectangle bounds = scheme->tileToRectangle(rootKey);
    const Mat4 transform = Mat4::scale(Vec3(1.00001, 1.00001, 1.00001));
    putLoadedGltfTerrainTile(
        transformedTiles,
        rootKey,
        bounds,
        makeTerrainGltfTriangle(bounds, 10.0, 20.0, 30.0),
        transform);
    putLoadedGltfTerrainTile(
        bakedTiles,
        rootKey,
        bounds,
        makeTransformedTerrainGltfTriangle(
            bounds,
            10.0,
            20.0,
            30.0,
            transform));

    const double longitude = bounds.west() + bounds.width() * 0.25;
    const double latitude = bounds.south() + bounds.height() * 0.25;
    const float transformedHeight = LoadedTerrainHeightSampler::sampleHeight(
        transformedTiles,
        terrainCache,
        longitude,
        latitude,
        LoadedTerrainHeightCacheMode::ContentOwnedTerrainOnly);
    const float bakedHeight = LoadedTerrainHeightSampler::sampleHeight(
        bakedTiles,
        terrainCache,
        longitude,
        latitude,
        LoadedTerrainHeightCacheMode::ContentOwnedTerrainOnly);

    EXPECT_GT(transformedHeight, 17.5f);
    EXPECT_NEAR(transformedHeight, bakedHeight, 1e-4f);
}
