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
    std::unique_ptr<GltfModel> model) {
    auto tile = std::make_unique<TilesetTile>(key, bounds);
    tile->content.renderContent.prepareGltfContent(
        std::move(model),
        Mat4::identity());
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
            latitude),
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
            latitude),
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
            latitude),
        1e-4f);
}
