#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/S2CellID.h"
#include "earth_engine/tiling/TileSoftwareOcclusionPolicy.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"

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
