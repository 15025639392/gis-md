#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/S2CellID.h"
#include "earth_engine/tiling/TileSoftwareOcclusionPolicy.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"

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

TEST(TileSoftwareOcclusionTest, DefaultTilesetOcclusionCullsFarSideTile) {
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
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
