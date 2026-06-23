#include <gtest/gtest.h>

#include "earth_engine/tiling/TileSurface.h"
#include "earth_engine/tiling/TileContentCacheManager.h"
#include "earth_engine/tiling/TileContentLifecycleManager.h"
#include "earth_engine/tiling/TileContentResourceInvalidator.h"
#include "earth_engine/tiling/TileLoadQueue.h"
#include "earth_engine/tiling/TileMeshPreparationManager.h"
#include "earth_engine/tiling/TileSurfaceMeshEnsurer.h"
#include "earth_engine/tiling/TileUpsampleSourcePreparer.h"

#include <memory>

using namespace earth_engine;

TEST(TileSurfaceMeshEnsurerTest,
     ContentOwnedTerrainModeRejectsLegacyFallbacks) {
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
                false},
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
                true},
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

TEST(TileSurfaceMeshEnsurerTest,
     ContentOwnedUpsampleSourceRejectsHeightmapSurfaceMeshAncestor) {
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

    EXPECT_EQ(nullptr,
              TileUpsampleSourcePreparer::findSourceTile(
                  child,
                  false,
                  true,
                  false));
}

TEST(TileSurfaceMeshEnsurerTest,
     HeightmapSurfacePathStillAcceptsSurfaceMeshAncestor) {
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

    EXPECT_EQ(&parent,
              TileUpsampleSourcePreparer::findSourceTile(
                  child,
                  false,
                  true,
                  true));
}

TEST(TileSurfaceMeshEnsurerTest,
     ContentTerrainManagerPreparationOnlyClearsLegacyResidue) {
    TileContentLifecycleManager lifecycle;
    TileContentCacheManager cache;
    uint64_t resourceRevision = 1;
    TileContentResourceInvalidator invalidator(resourceRevision, cache);
    TileLoadQueue loadQueue;
    std::vector<ActivatedRasterOverlay*> overlays;
    TileMeshPreparationManager manager(
        lifecycle,
        invalidator,
        loadQueue,
        true,
        false,
        nullptr,
        overlays);

    TilesetTile tile(
        TileKey{"Geographic-TMS", 1, 0, 0},
        Rectangle::fromDegrees(-180.0, 0.0, 0.0, 90.0));
    tile.content.renderContent.setSurfaceMesh(
        std::make_unique<SurfaceTileMesh>(
            TileSurface::buildEllipsoidMesh(tile.bounds, 4)));
    auto retainedHeightmap = std::make_unique<DecodedHeightmap>();
    retainedHeightmap->heights.resize(4, 12.0f);
    tile.content.renderContent.setRetainedHeightmap(
        std::move(retainedHeightmap));

    manager.prepareRenderableTile(tile);

    EXPECT_FALSE(tile.content.renderContent.hasSurfaceMesh());
    EXPECT_FALSE(tile.content.renderContent.hasRetainedHeightmap());
    EXPECT_EQ(2u, resourceRevision);
    EXPECT_TRUE(cache.cacheBytesDirty());
}
