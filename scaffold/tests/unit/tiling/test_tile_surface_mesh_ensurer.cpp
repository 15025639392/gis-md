#include <gtest/gtest.h>

#include "earth_engine/tiling/TileSurface.h"
#include "earth_engine/tiling/TileSurfaceMeshEnsurer.h"

#include <memory>

using namespace earth_engine;

TEST(TileSurfaceMeshEnsurerTest,
     ContentOwnedTerrainModeOverridesLegacyFallbackFlags) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle::fromDegrees(-180.0, -90.0, 180.0, 90.0));
    parent.content.renderContent.setSurfaceMesh(
        std::make_unique<SurfaceTileMesh>(
            TileSurface::buildEllipsoidMesh(parent.bounds, 4)));
    parent.markRenderContentDone();

    TilesetTile child(
        TileKey{"Geographic-TMS", 1, 0, 0},
        Rectangle::fromDegrees(-180.0, 0.0, 0.0, 90.0),
        &parent);
    child.content.markTerrainAvailabilityUpsample();

    bool ancestorEnsured = false;
    bool completenessChecked = false;
    const TileSurfaceMeshEnsureResult result =
        TileSurfaceMeshEnsurer::ensure(
            TileSurfaceMeshEnsureInput{
                child,
                nullptr,
                nullptr,
                true,
                true,
                true,
                TileMeshLegacyHeightmapMode::ContentOwnedTerrainOnly},
            [](const TileKey&, DecodedHeightmap*) {},
            [&parent](const TilesetTile&, bool) -> const TilesetTile* {
                return &parent;
            },
            [&ancestorEnsured](TilesetTile&) {
                ancestorEnsured = true;
            },
            [&completenessChecked](const TilesetTile&) {
                completenessChecked = true;
                return true;
            });

    EXPECT_FALSE(result.resourcesDirty);
    EXPECT_FALSE(child.content.renderContent.hasSurfaceMesh());
    EXPECT_FALSE(ancestorEnsured);
    EXPECT_FALSE(completenessChecked);
    EXPECT_NE(TileLoadState::Done, child.content.loadState);
}

TEST(TileSurfaceMeshEnsurerTest,
     LegacyModeStillAllowsAncestorSurfaceUpsample) {
    TilesetTile parent(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle::fromDegrees(-180.0, -90.0, 180.0, 90.0));
    parent.content.renderContent.setSurfaceMesh(
        std::make_unique<SurfaceTileMesh>(
            TileSurface::buildEllipsoidMesh(parent.bounds, 4)));
    parent.markRenderContentDone();

    TilesetTile child(
        TileKey{"Geographic-TMS", 1, 0, 0},
        Rectangle::fromDegrees(-180.0, 0.0, 0.0, 90.0),
        &parent);
    child.content.markTerrainAvailabilityUpsample();

    bool completenessChecked = false;
    const TileSurfaceMeshEnsureResult result =
        TileSurfaceMeshEnsurer::ensure(
            TileSurfaceMeshEnsureInput{
                child,
                nullptr,
                nullptr,
                true,
                false,
                true,
                TileMeshLegacyHeightmapMode::Include},
            [](const TileKey&, DecodedHeightmap*) {},
            [&parent](const TilesetTile&, bool) -> const TilesetTile* {
                return &parent;
            },
            [](TilesetTile&) {},
            [&completenessChecked](const TilesetTile&) {
                completenessChecked = true;
                return true;
            });

    EXPECT_TRUE(result.resourcesDirty);
    EXPECT_TRUE(child.content.renderContent.hasSurfaceMesh());
    EXPECT_TRUE(completenessChecked);
    EXPECT_EQ(TileLoadState::Done, child.content.loadState);
}
