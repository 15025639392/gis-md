#include <gtest/gtest.h>

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/S2CellID.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/math/MathUtils.h"
#include "earth_engine/tiling/SurfaceTile.h"
#include "earth_engine/tiling/TileSoftwareOcclusionPolicy.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"

#include <array>
#include <optional>

using namespace earth_engine;

namespace earth_engine {
struct TilesetTestAccess {
    static TilesetTile* ensureTile(Tileset& tileset, const TileKey& key) {
        return tileset.contentAccess_.ensureTile(key);
    }

    static void setLastCamera(
        Tileset& tileset,
        const Vec3& position,
        const Vec3& direction) {
        tileset.lastCameraPosition_ = position;
        tileset.lastCameraDirection_ = direction;
    }

    static TileOcclusionState checkOcclusion(
        const Tileset& tileset,
        const TilesetTile& tile) {
        return tileset.checkOcclusion(tile);
    }
};
} // namespace earth_engine

namespace {

std::unique_ptr<GltfModel> makeQuadTerrainGltfModel(
    const Rectangle& rectangle) {
    auto model = std::make_unique<GltfModel>();
    const Vec3 nodeOrigin(100.0, 200.0, 300.0);
    GltfNodeRuntime rootNode;
    rootNode.baseLocalTransform = Mat4::translation(nodeOrigin);
    rootNode.localTransform = rootNode.baseLocalTransform;
    rootNode.globalTransform = rootNode.baseLocalTransform;
    rootNode.mesh = 0;
    rootNode.hasMatrix = true;
    rootNode.baseTranslation = {nodeOrigin.x(), nodeOrigin.y(), nodeOrigin.z()};
    rootNode.translation = rootNode.baseTranslation;
    model->nodes.push_back(rootNode);
    model->sceneRootNodes.push_back(0);
    GltfPrimitive primitive;
    primitive.vertices.resize(4);
    primitive.vertices[0].positionEcef = nodeOrigin + Vec3(0.0, 0.0, 0.0);
    primitive.vertices[1].positionEcef = nodeOrigin + Vec3(2.0, 0.0, 0.0);
    primitive.vertices[2].positionEcef = nodeOrigin + Vec3(0.0, 2.0, 0.0);
    primitive.vertices[3].positionEcef = nodeOrigin + Vec3(2.0, 2.0, 0.0);
    for (SurfaceVertex& vertex : primitive.vertices) {
        vertex.normalEcef = Vec3::unitZ();
    }
    primitive.vertices[0].uv = {0.0f, 0.0f};
    primitive.vertices[1].uv = {1.0f, 0.0f};
    primitive.vertices[2].uv = {0.0f, 1.0f};
    primitive.vertices[3].uv = {1.0f, 1.0f};
    primitive.vertexTexCoords[0] = {
        std::array<float, 2>{0.0f, 0.0f},
        std::array<float, 2>{1.0f, 0.0f},
        std::array<float, 2>{0.0f, 1.0f},
        std::array<float, 2>{1.0f, 1.0f}};
    primitive.indices = {0, 1, 2, 1, 3, 2};
    primitive.runtime.baseVertices = primitive.vertices;
    for (SurfaceVertex& vertex : primitive.runtime.baseVertices) {
        vertex.positionEcef = vertex.positionEcef - nodeOrigin;
    }
    primitive.runtime.nodeIndex = 0;
    primitive.runtime.hasNormals = true;
    model->primitives.push_back(std::move(primitive));
    model->rasterOverlayDetails.setGeographicRectangle(rectangle);
    return model;
}

} // namespace

TEST(TileSoftwareOcclusionTest, DefaultTilesetOcclusionCullsFarSideTile) {
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{});

    const TileKey farSideKey{"Geographic-TMS", 2, 7, 1};
    TilesetTile* farSide = TilesetTestAccess::ensureTile(tileset, farSideKey);
    ASSERT_NE(farSide, nullptr);

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 cameraPosition(
        ellipsoid.semiMajorAxis() + 1000000.0,
        0.0,
        0.0);
    TilesetTestAccess::setLastCamera(
        tileset,
        cameraPosition,
        Vec3(-1.0, 0.0, 0.0));

    EXPECT_EQ(
        TilesetTestAccess::checkOcclusion(tileset, *farSide),
        TileOcclusionState::Occluded);
}

TEST(TileSoftwareOcclusionTest, KeepsNonBoxVolumeUnderCameraVisible) {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey key{"Geographic-TMS", 2, 4, 2};
    TilesetTile tile;
    tile.key = key;
    tile.bounds = scheme->tileToRectangle(key);
    tile.boundingVolume = TileBoundingVolume::fromS2Cell(
        S2CellBoundingVolume(S2CellID::fromToken("1"), 0.0, 100000.0));

    const Vec3 cameraPosition =
        Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic::fromRadians(0.0, 0.0, 1000000.0));

    EXPECT_EQ(
        TileSoftwareOcclusionPolicy::check(tile, cameraPosition),
        TileOcclusionState::NotOccluded);
}

TEST(TileSoftwareOcclusionTest, UsesExplicitVolumeRectangleForUnderCamera) {
    TilesetTile tile;
    tile.key = TileKey{"Geographic-TMS", 4, 0, 0};
    tile.bounds = Rectangle::fromDegrees(170.0, -10.0, 179.0, 10.0);
    tile.boundingVolume = TileBoundingVolume::fromS2Cell(
        S2CellBoundingVolume(S2CellID::fromToken("1"), 0.0, 100000.0));

    const std::optional<Rectangle> volumeRectangle =
        tile.boundingVolume->estimateGlobeRectangle();
    ASSERT_TRUE(volumeRectangle.has_value());

    const double cameraLongitude =
        (volumeRectangle->west() + volumeRectangle->east()) * 0.5;
    const double cameraLatitude =
        (volumeRectangle->south() + volumeRectangle->north()) * 0.5;
    ASSERT_TRUE(volumeRectangle->contains(cameraLongitude, cameraLatitude));
    ASSERT_FALSE(tile.bounds.contains(cameraLongitude, cameraLatitude));

    const Vec3 cameraPosition =
        Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic::fromRadians(
                cameraLongitude,
                cameraLatitude,
                1000000.0));

    EXPECT_EQ(
        TileSoftwareOcclusionPolicy::check(tile, cameraPosition),
        TileOcclusionState::NotOccluded);
}

TEST(TileSoftwareOcclusionTest, UsesExplicitRegionHeightForFallbackSamples) {
    TilesetTile tile;
    tile.key = TileKey{"Geographic-TMS", 4, 0, 0};
    tile.bounds = Rectangle::fromDegrees(140.0, -1.0, 141.0, 1.0);
    tile.boundingVolume =
        TileBoundingVolume::fromRegion(
            Rectangle::fromDegrees(34.8, -0.05, 35.2, 0.05),
            0.0,
            2000000.0);

    const Vec3 cameraPosition =
        Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic::fromRadians(0.0, 0.0, 1000000.0));

    EXPECT_EQ(
        TileSoftwareOcclusionPolicy::check(tile, cameraPosition),
        TileOcclusionState::NotOccluded);
}

TEST(
    TileSoftwareOcclusionTest,
    DoesNotInflateExplicitRegionToDefaultTerrainHeight) {
    TilesetTile tile;
    tile.key = TileKey{"Geographic-TMS", 4, 0, 0};
    tile.bounds = Rectangle::fromDegrees(140.0, -1.0, 141.0, 1.0);
    tile.boundingVolume =
        TileBoundingVolume::fromRegion(
            Rectangle::fromDegrees(30.35, -0.01, 30.45, 0.01),
            0.0,
            0.0);

    const Vec3 cameraPosition =
        Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic::fromRadians(0.0, 0.0, 1000000.0));

    EXPECT_EQ(
        TileSoftwareOcclusionPolicy::check(tile, cameraPosition),
        TileOcclusionState::Occluded);
}

TEST(TileSoftwareOcclusionTest, PreservesNegativeExplicitRegionHeight) {
    TilesetTile tile;
    tile.key = TileKey{"Geographic-TMS", 4, 0, 0};
    tile.bounds = Rectangle::fromDegrees(140.0, -1.0, 141.0, 1.0);
    tile.boundingVolume =
        TileBoundingVolume::fromRegion(
            Rectangle::fromDegrees(30.0, -0.01, 30.1, 0.01),
            -1000.0,
            -1000.0);

    const Vec3 cameraPosition =
        Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic::fromRadians(0.0, 0.0, 1000000.0));

    EXPECT_EQ(
        TileSoftwareOcclusionPolicy::check(tile, cameraPosition),
        TileOcclusionState::Occluded);
}

TEST(TileSoftwareOcclusionTest, PreservesNegativeExplicitS2Height) {
    TilesetTile tile;
    tile.key = TileKey{"Geographic-TMS", 4, 0, 0};
    tile.bounds = Rectangle::fromDegrees(140.0, -1.0, 141.0, 1.0);
    tile.boundingVolume = TileBoundingVolume::fromS2Cell(
        S2CellBoundingVolume(
            S2CellID::fromQuadtreeTileID(0, 4, 14, 7),
            -1000.0,
            -1000.0));

    const std::optional<Rectangle> volumeRectangle =
        tile.boundingVolume->estimateGlobeRectangle();
    ASSERT_TRUE(volumeRectangle.has_value());
    ASSERT_FALSE(volumeRectangle->contains(0.0, 0.0));

    const Vec3 cameraPosition =
        Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic::fromRadians(0.0, 0.0, 1000000.0));

    EXPECT_EQ(
        TileSoftwareOcclusionPolicy::check(tile, cameraPosition),
        TileOcclusionState::Occluded);
}

TEST(TileSoftwareOcclusionTest, UsesQuantizedMeshHorizonPoint) {
    TilesetTile tile;
    tile.key = TileKey{"Geographic-TMS", 4, 0, 0};
    tile.bounds = Rectangle::fromDegrees(10.0, -0.01, 10.1, 0.01);
    auto gltfModel = makeQuadTerrainGltfModel(tile.bounds);
    tile.content.renderContent.prepareGltfContent(
        std::move(gltfModel), Mat4::identity());
    tile.content.renderContent.setHorizonOcclusionPoint(
        Vec3(std::cos(MathUtils::degreesToRadians(60.0)),
             std::sin(MathUtils::degreesToRadians(60.0)),
             0.0));
    tile.content.renderContent.setTerrainRenderContent(true);
    tile.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    tile.content.renderContent.markRenderContentReady();

    const Vec3 cameraPosition =
        Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic::fromRadians(0.0, 0.0, 1000000.0));

    EXPECT_EQ(
        TileSoftwareOcclusionPolicy::check(tile, cameraPosition),
        TileOcclusionState::Occluded);
}

TEST(TileSoftwareOcclusionTest, KeepsVisibleQuantizedMeshHorizonPoint) {
    TilesetTile tile;
    tile.key = TileKey{"Geographic-TMS", 4, 0, 0};
    tile.bounds = Rectangle::fromDegrees(10.0, -0.01, 10.1, 0.01);
    auto gltfModel = makeQuadTerrainGltfModel(tile.bounds);
    tile.content.renderContent.prepareGltfContent(
        std::move(gltfModel), Mat4::identity());
    tile.content.renderContent.setHorizonOcclusionPoint(
        Vec3(std::cos(MathUtils::degreesToRadians(30.0)),
             std::sin(MathUtils::degreesToRadians(30.0)),
             0.0));
    tile.content.renderContent.setTerrainRenderContent(true);
    tile.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    tile.content.renderContent.markRenderContentReady();

    const Vec3 cameraPosition =
        Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic::fromRadians(0.0, 0.0, 1000000.0));

    EXPECT_EQ(
        TileSoftwareOcclusionPolicy::check(tile, cameraPosition),
        TileOcclusionState::NotOccluded);
}

TEST(TileSoftwareOcclusionTest, UsesZeroQuantizedMeshHorizonPoint) {
    TilesetTile tile;
    tile.key = TileKey{"Geographic-TMS", 4, 0, 0};
    tile.bounds = Rectangle::fromDegrees(10.0, -0.01, 10.1, 0.01);
    auto gltfModel = makeQuadTerrainGltfModel(tile.bounds);
    tile.content.renderContent.prepareGltfContent(
        std::move(gltfModel), Mat4::identity());
    tile.content.renderContent.setHorizonOcclusionPoint(Vec3::zero());
    tile.content.renderContent.setTerrainRenderContent(true);
    tile.content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    tile.content.renderContent.markRenderContentReady();

    const Vec3 cameraPosition =
        Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic::fromRadians(0.0, 0.0, 1000000.0));

    EXPECT_EQ(
        TileSoftwareOcclusionPolicy::check(tile, cameraPosition),
        TileOcclusionState::Occluded);
}
