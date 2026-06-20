#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Transforms.h"
#include "earth_engine/core/geodesy/S2CellBoundingVolume.h"
#include "earth_engine/core/geodesy/S2CellID.h"
#include "earth_engine/core/math/BoundingCylinderRegion.h"
#include "earth_engine/core/math/MathUtils.h"
#include "earth_engine/core/math/OrientedBoundingBox.h"
#include "earth_engine/tiling/ImplicitTileIdUtilities.h"
#include "earth_engine/tiling/TileBoundingVolume.h"

#include <vector>

using namespace earth_engine;

namespace {

void expectDVec2Near(const glm::dvec2& actual,
                     const glm::dvec2& expected,
                     double epsilon = MathUtils::Epsilon6) {
    EXPECT_NEAR(expected.x, actual.x, epsilon);
    EXPECT_NEAR(expected.y, actual.y, epsilon);
}

} // namespace

TEST(ImplicitTileIdUtilitiesTest, QuadtreeChildrenMatchCesiumNativeOrder) {
    const TileKey parent{"Geographic-TMS", 11, 2, 3};

    const std::vector<TileKey> children =
        ImplicitTileIdUtilities::children(parent);

    const std::vector<TileKey> expected{
        TileKey{"Geographic-TMS", 12, 4, 6},
        TileKey{"Geographic-TMS", 12, 5, 6},
        TileKey{"Geographic-TMS", 12, 4, 7},
        TileKey{"Geographic-TMS", 12, 5, 7}};
    EXPECT_EQ(expected, children);
}

TEST(ImplicitTileIdUtilitiesTest, OctreeChildrenMatchCesiumNativeOrder) {
    const OctreeTileID parent{11, 2, 3, 4};

    const std::vector<OctreeTileID> children =
        ImplicitTileIdUtilities::children(parent);

    const std::vector<OctreeTileID> expected{
        OctreeTileID{12, 4, 6, 8},
        OctreeTileID{12, 5, 6, 8},
        OctreeTileID{12, 4, 7, 8},
        OctreeTileID{12, 5, 7, 8},
        OctreeTileID{12, 4, 6, 9},
        OctreeTileID{12, 5, 6, 9},
        OctreeTileID{12, 4, 7, 9},
        OctreeTileID{12, 5, 7, 9}};
    EXPECT_EQ(expected, children);
}

TEST(ImplicitTileIdUtilitiesTest, ResolveUrlMatchesCesiumNative) {
    EXPECT_EQ("https://example.com/tiles/11/2/3",
              ImplicitTileIdUtilities::resolveUrl(
                  "https://example.com",
                  "tiles/{level}/{x}/{y}",
                  TileKey{"Geographic-TMS", 11, 2, 3}));

    EXPECT_EQ("https://example.com/tiles/11/2/3/4",
              ImplicitTileIdUtilities::resolveUrl(
                  "https://example.com",
                  "tiles/{level}/{x}/{y}/{z}",
                  OctreeTileID{11, 2, 3, 4}));
}

TEST(ImplicitTileIdUtilitiesTest, ResolveUrlPreservesTemplateEdgeCases) {
    EXPECT_EQ("https://example.com/base/tiles/11/unknown/3",
              ImplicitTileIdUtilities::resolveUrl(
                  "https://example.com/base/tileset.json",
                  "tiles/{level}/{unknown}/{y}",
                  TileKey{"Geographic-TMS", 11, 2, 3}));

    EXPECT_EQ("https://example.com/tiles/{level",
              ImplicitTileIdUtilities::resolveUrl(
                  "https://example.com/base/tileset.json",
                  "../tiles/{level",
                  TileKey{"Geographic-TMS", 11, 2, 3}));

    EXPECT_EQ("https://example.com/base/tiles/11/3",
              ImplicitTileIdUtilities::resolveUrl(
                  "https://example.com/base/tileset.json",
                  "tiles/{level}/{}/{y}",
                  TileKey{"Geographic-TMS", 11, 2, 3}));

    EXPECT_EQ("https://example.com/base/tiles/11}/3",
              ImplicitTileIdUtilities::resolveUrl(
                  "https://example.com/base/tileset.json",
                  "tiles/{level}}/{y}",
                  TileKey{"Geographic-TMS", 11, 2, 3}));

    EXPECT_EQ("https://example.com/base/tileset.json",
              ImplicitTileIdUtilities::resolveUrl(
                  "https://example.com/base/tileset.json?token=base#section",
                  "",
                  TileKey{"Geographic-TMS", 11, 2, 3}));
}

TEST(ImplicitTileIdUtilitiesTest, ComputeObbQuadtreeBoundingVolumeMatchesCesiumNative) {
    const OrientedBoundingBox root(Vec3(1.0, 2.0, 3.0),
                                  Vec3(10.0, 0.0, 0.0),
                                  Vec3(0.0, 10.0, 0.0),
                                  Vec3(0.0, 0.0, 10.0));

    const OrientedBoundingBox l1x0y0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            TileKey{"Geographic-TMS", 1, 0, 0});
    EXPECT_EQ(Vec3(-4.0, -3.0, 3.0), l1x0y0.getCenter());
    EXPECT_EQ(Vec3(10.0, 10.0, 20.0), l1x0y0.getLengths());

    const OrientedBoundingBox l1x1y0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            TileKey{"Geographic-TMS", 1, 1, 0});
    EXPECT_EQ(Vec3(6.0, -3.0, 3.0), l1x1y0.getCenter());
    EXPECT_EQ(Vec3(10.0, 10.0, 20.0), l1x1y0.getLengths());

    const OrientedBoundingBox l1x0y1 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            TileKey{"Geographic-TMS", 1, 0, 1});
    EXPECT_EQ(Vec3(-4.0, 7.0, 3.0), l1x0y1.getCenter());
    EXPECT_EQ(Vec3(10.0, 10.0, 20.0), l1x0y1.getLengths());
}

TEST(ImplicitTileIdUtilitiesTest, ComputeObbOctreeBoundingVolumeMatchesCesiumNative) {
    const OrientedBoundingBox root(Vec3(1.0, 2.0, 3.0),
                                  Vec3(10.0, 0.0, 0.0),
                                  Vec3(0.0, 10.0, 0.0),
                                  Vec3(0.0, 0.0, 10.0));

    const OrientedBoundingBox l1x0y0z0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 0, 0, 0});
    EXPECT_EQ(Vec3(-4.0, -3.0, -2.0), l1x0y0z0.getCenter());
    EXPECT_EQ(Vec3(10.0, 10.0, 10.0), l1x0y0z0.getLengths());

    const OrientedBoundingBox l1x1y0z0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 1, 0, 0});
    EXPECT_EQ(Vec3(6.0, -3.0, -2.0), l1x1y0z0.getCenter());
    EXPECT_EQ(Vec3(10.0, 10.0, 10.0), l1x1y0z0.getLengths());

    const OrientedBoundingBox l1x0y1z0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 0, 1, 0});
    EXPECT_EQ(Vec3(-4.0, 7.0, -2.0), l1x0y1z0.getCenter());
    EXPECT_EQ(Vec3(10.0, 10.0, 10.0), l1x0y1z0.getLengths());

    const OrientedBoundingBox l1x0y0z1 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 0, 0, 1});
    EXPECT_EQ(Vec3(-4.0, -3.0, 8.0), l1x0y0z1.getCenter());
    EXPECT_EQ(Vec3(10.0, 10.0, 10.0), l1x0y0z1.getLengths());
}

TEST(ImplicitTileIdUtilitiesTest,
     ComputeRegionQuadtreeBoundingVolumeMatchesCesiumNative) {
    const TileBoundingVolume root =
        TileBoundingVolume::fromRegion(Rectangle(1.0, 2.0, 3.0, 4.0),
                                       10.0,
                                       20.0);

    const TileBoundingVolume l1x0y0 =
        ImplicitTileIdUtilities::computeRegionBoundingVolume(
            root,
            TileKey{"Geographic-TMS", 1, 0, 0});
    EXPECT_EQ(TileBoundingVolumeKind::Region, l1x0y0.kind);
    EXPECT_DOUBLE_EQ(1.0, l1x0y0.region.west());
    EXPECT_DOUBLE_EQ(2.0, l1x0y0.region.south());
    EXPECT_DOUBLE_EQ(2.0, l1x0y0.region.east());
    EXPECT_DOUBLE_EQ(3.0, l1x0y0.region.north());
    EXPECT_DOUBLE_EQ(10.0, l1x0y0.minimumHeight);
    EXPECT_DOUBLE_EQ(20.0, l1x0y0.maximumHeight);

    const TileBoundingVolume l1x1y0 =
        ImplicitTileIdUtilities::computeRegionBoundingVolume(
            root,
            TileKey{"Geographic-TMS", 1, 1, 0});
    EXPECT_EQ(TileBoundingVolumeKind::Region, l1x1y0.kind);
    EXPECT_DOUBLE_EQ(2.0, l1x1y0.region.west());
    EXPECT_DOUBLE_EQ(2.0, l1x1y0.region.south());
    EXPECT_DOUBLE_EQ(3.0, l1x1y0.region.east());
    EXPECT_DOUBLE_EQ(3.0, l1x1y0.region.north());
    EXPECT_DOUBLE_EQ(10.0, l1x1y0.minimumHeight);
    EXPECT_DOUBLE_EQ(20.0, l1x1y0.maximumHeight);

    const TileBoundingVolume l1x0y1 =
        ImplicitTileIdUtilities::computeRegionBoundingVolume(
            root,
            TileKey{"Geographic-TMS", 1, 0, 1});
    EXPECT_EQ(TileBoundingVolumeKind::Region, l1x0y1.kind);
    EXPECT_DOUBLE_EQ(1.0, l1x0y1.region.west());
    EXPECT_DOUBLE_EQ(3.0, l1x0y1.region.south());
    EXPECT_DOUBLE_EQ(2.0, l1x0y1.region.east());
    EXPECT_DOUBLE_EQ(4.0, l1x0y1.region.north());
    EXPECT_DOUBLE_EQ(10.0, l1x0y1.minimumHeight);
    EXPECT_DOUBLE_EQ(20.0, l1x0y1.maximumHeight);
}

TEST(ImplicitTileIdUtilitiesTest,
     ComputeRegionOctreeBoundingVolumeMatchesCesiumNative) {
    const TileBoundingVolume root =
        TileBoundingVolume::fromRegion(Rectangle(1.0, 2.0, 3.0, 4.0),
                                       10.0,
                                       20.0);

    const TileBoundingVolume l1x0y0z0 =
        ImplicitTileIdUtilities::computeRegionBoundingVolume(
            root,
            OctreeTileID{1, 0, 0, 0});
    EXPECT_EQ(TileBoundingVolumeKind::Region, l1x0y0z0.kind);
    EXPECT_DOUBLE_EQ(1.0, l1x0y0z0.region.west());
    EXPECT_DOUBLE_EQ(2.0, l1x0y0z0.region.south());
    EXPECT_DOUBLE_EQ(2.0, l1x0y0z0.region.east());
    EXPECT_DOUBLE_EQ(3.0, l1x0y0z0.region.north());
    EXPECT_DOUBLE_EQ(10.0, l1x0y0z0.minimumHeight);
    EXPECT_DOUBLE_EQ(15.0, l1x0y0z0.maximumHeight);

    const TileBoundingVolume l1x1y0z0 =
        ImplicitTileIdUtilities::computeRegionBoundingVolume(
            root,
            OctreeTileID{1, 1, 0, 0});
    EXPECT_EQ(TileBoundingVolumeKind::Region, l1x1y0z0.kind);
    EXPECT_DOUBLE_EQ(2.0, l1x1y0z0.region.west());
    EXPECT_DOUBLE_EQ(2.0, l1x1y0z0.region.south());
    EXPECT_DOUBLE_EQ(3.0, l1x1y0z0.region.east());
    EXPECT_DOUBLE_EQ(3.0, l1x1y0z0.region.north());
    EXPECT_DOUBLE_EQ(10.0, l1x1y0z0.minimumHeight);
    EXPECT_DOUBLE_EQ(15.0, l1x1y0z0.maximumHeight);

    const TileBoundingVolume l1x0y1z0 =
        ImplicitTileIdUtilities::computeRegionBoundingVolume(
            root,
            OctreeTileID{1, 0, 1, 0});
    EXPECT_EQ(TileBoundingVolumeKind::Region, l1x0y1z0.kind);
    EXPECT_DOUBLE_EQ(1.0, l1x0y1z0.region.west());
    EXPECT_DOUBLE_EQ(3.0, l1x0y1z0.region.south());
    EXPECT_DOUBLE_EQ(2.0, l1x0y1z0.region.east());
    EXPECT_DOUBLE_EQ(4.0, l1x0y1z0.region.north());
    EXPECT_DOUBLE_EQ(10.0, l1x0y1z0.minimumHeight);
    EXPECT_DOUBLE_EQ(15.0, l1x0y1z0.maximumHeight);

    const TileBoundingVolume l1x0y0z1 =
        ImplicitTileIdUtilities::computeRegionBoundingVolume(
            root,
            OctreeTileID{1, 0, 0, 1});
    EXPECT_EQ(TileBoundingVolumeKind::Region, l1x0y0z1.kind);
    EXPECT_DOUBLE_EQ(1.0, l1x0y0z1.region.west());
    EXPECT_DOUBLE_EQ(2.0, l1x0y0z1.region.south());
    EXPECT_DOUBLE_EQ(2.0, l1x0y0z1.region.east());
    EXPECT_DOUBLE_EQ(3.0, l1x0y0z1.region.north());
    EXPECT_DOUBLE_EQ(15.0, l1x0y0z1.minimumHeight);
    EXPECT_DOUBLE_EQ(20.0, l1x0y0z1.maximumHeight);
}

TEST(ImplicitTileIdUtilitiesTest,
     ComputeS2QuadtreeBoundingVolumeMatchesCesiumNative) {
    const S2CellBoundingVolume root(
        S2CellID::fromQuadtreeTileID(1, 0, 0, 0),
        10.0,
        20.0);

    const S2CellBoundingVolume l1x0y0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            TileKey{"S2", 1, 0, 0});
    EXPECT_EQ(1, l1x0y0.getCellID().getFace());
    EXPECT_EQ(S2CellID::fromQuadtreeTileID(1, 1, 0, 0).getID(),
              l1x0y0.getCellID().getID());
    EXPECT_DOUBLE_EQ(10.0, l1x0y0.getMinimumHeight());
    EXPECT_DOUBLE_EQ(20.0, l1x0y0.getMaximumHeight());

    const S2CellBoundingVolume l1x1y0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            TileKey{"S2", 1, 1, 0});
    EXPECT_EQ(1, l1x1y0.getCellID().getFace());
    EXPECT_EQ(S2CellID::fromQuadtreeTileID(1, 1, 1, 0).getID(),
              l1x1y0.getCellID().getID());
    EXPECT_DOUBLE_EQ(10.0, l1x1y0.getMinimumHeight());
    EXPECT_DOUBLE_EQ(20.0, l1x1y0.getMaximumHeight());

    const S2CellBoundingVolume l1x0y1 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            TileKey{"S2", 1, 0, 1});
    EXPECT_EQ(1, l1x0y1.getCellID().getFace());
    EXPECT_EQ(S2CellID::fromQuadtreeTileID(1, 1, 0, 1).getID(),
              l1x0y1.getCellID().getID());
    EXPECT_DOUBLE_EQ(10.0, l1x0y1.getMinimumHeight());
    EXPECT_DOUBLE_EQ(20.0, l1x0y1.getMaximumHeight());
}

TEST(ImplicitTileIdUtilitiesTest,
     ComputeS2OctreeBoundingVolumeMatchesCesiumNative) {
    const S2CellBoundingVolume root(
        S2CellID::fromQuadtreeTileID(1, 0, 0, 0),
        10.0,
        20.0);

    const S2CellBoundingVolume l1x0y0z0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 0, 0, 0});
    EXPECT_EQ(1, l1x0y0z0.getCellID().getFace());
    EXPECT_EQ(S2CellID::fromQuadtreeTileID(1, 1, 0, 0).getID(),
              l1x0y0z0.getCellID().getID());
    EXPECT_DOUBLE_EQ(10.0, l1x0y0z0.getMinimumHeight());
    EXPECT_DOUBLE_EQ(15.0, l1x0y0z0.getMaximumHeight());

    const S2CellBoundingVolume l1x1y0z0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 1, 0, 0});
    EXPECT_EQ(1, l1x1y0z0.getCellID().getFace());
    EXPECT_EQ(S2CellID::fromQuadtreeTileID(1, 1, 1, 0).getID(),
              l1x1y0z0.getCellID().getID());
    EXPECT_DOUBLE_EQ(10.0, l1x1y0z0.getMinimumHeight());
    EXPECT_DOUBLE_EQ(15.0, l1x1y0z0.getMaximumHeight());

    const S2CellBoundingVolume l1x0y1z0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 0, 1, 0});
    EXPECT_EQ(1, l1x0y1z0.getCellID().getFace());
    EXPECT_EQ(S2CellID::fromQuadtreeTileID(1, 1, 0, 1).getID(),
              l1x0y1z0.getCellID().getID());
    EXPECT_DOUBLE_EQ(10.0, l1x0y1z0.getMinimumHeight());
    EXPECT_DOUBLE_EQ(15.0, l1x0y1z0.getMaximumHeight());

    const S2CellBoundingVolume l1x0y0z1 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 0, 0, 1});
    EXPECT_EQ(1, l1x0y0z1.getCellID().getFace());
    EXPECT_EQ(S2CellID::fromQuadtreeTileID(1, 1, 0, 0).getID(),
              l1x0y0z1.getCellID().getID());
    EXPECT_DOUBLE_EQ(15.0, l1x0y0z1.getMinimumHeight());
    EXPECT_DOUBLE_EQ(20.0, l1x0y0z1.getMaximumHeight());
}

TEST(ImplicitTileIdUtilitiesTest,
     ComputeTileBoundingVolumeQuadtreeDispatchesByKindLikeCesiumNative) {
    const TileKey tileID{"Geographic-TMS", 1, 0, 1};

    const TileBoundingVolume box = TileBoundingVolume::fromBox(
        Vec3(1.0, 2.0, 3.0),
        Vec3(10.0, 0.0, 0.0),
        Vec3(0.0, 10.0, 0.0),
        Vec3(0.0, 0.0, 10.0));
    const TileBoundingVolume boxChild =
        ImplicitTileIdUtilities::computeBoundingVolume(box, tileID);
    EXPECT_EQ(TileBoundingVolumeKind::Box, boxChild.kind);
    EXPECT_EQ(Vec3(-4.0, 7.0, 3.0), boxChild.box.getCenter());
    EXPECT_EQ(Vec3(10.0, 10.0, 20.0), boxChild.box.getLengths());

    const TileBoundingVolume region =
        TileBoundingVolume::fromRegion(Rectangle(1.0, 2.0, 3.0, 4.0),
                                       10.0,
                                       20.0);
    const TileBoundingVolume regionChild =
        ImplicitTileIdUtilities::computeBoundingVolume(region, tileID);
    EXPECT_EQ(TileBoundingVolumeKind::Region, regionChild.kind);
    EXPECT_DOUBLE_EQ(1.0, regionChild.region.west());
    EXPECT_DOUBLE_EQ(3.0, regionChild.region.south());
    EXPECT_DOUBLE_EQ(2.0, regionChild.region.east());
    EXPECT_DOUBLE_EQ(4.0, regionChild.region.north());
    EXPECT_DOUBLE_EQ(10.0, regionChild.minimumHeight);
    EXPECT_DOUBLE_EQ(20.0, regionChild.maximumHeight);

    const TileBoundingVolume s2 = TileBoundingVolume::fromS2Cell(
        S2CellBoundingVolume(
            S2CellID::fromQuadtreeTileID(1, 0, 0, 0),
            10.0,
            20.0));
    const TileBoundingVolume s2Child =
        ImplicitTileIdUtilities::computeBoundingVolume(s2, tileID);
    EXPECT_EQ(TileBoundingVolumeKind::S2Cell, s2Child.kind);
    EXPECT_EQ(S2CellID::fromQuadtreeTileID(1, 1, 0, 1).getID(),
              s2Child.s2Cell.getCellID().getID());
    EXPECT_DOUBLE_EQ(10.0, s2Child.s2Cell.getMinimumHeight());
    EXPECT_DOUBLE_EQ(20.0, s2Child.s2Cell.getMaximumHeight());

    const TileBoundingVolume cylinder = TileBoundingVolume::fromCylinderRegion(
        BoundingCylinderRegion(
            Vec3(1.0, 2.0, 3.0),
            glm::dquat(Transforms::Z_UP_TO_Y_UP().raw()),
            2.0,
            glm::dvec2(0.0, 1.0)));
    const TileBoundingVolume cylinderChild =
        ImplicitTileIdUtilities::computeBoundingVolume(cylinder, tileID);
    EXPECT_EQ(TileBoundingVolumeKind::CylinderRegion, cylinderChild.kind);
    EXPECT_EQ(cylinder.cylinderRegion.getHeight(),
              cylinderChild.cylinderRegion.getHeight());
    EXPECT_EQ(glm::dvec2(0.0, 0.5),
              cylinderChild.cylinderRegion.getRadialBounds());
    EXPECT_EQ(glm::dvec2(0.0, MathUtils::OnePi),
              cylinderChild.cylinderRegion.getAngularBounds());
}

TEST(ImplicitTileIdUtilitiesTest,
     ComputeTileBoundingVolumeOctreeDispatchesByKindLikeCesiumNative) {
    const OctreeTileID tileID{1, 0, 1, 1};

    const TileBoundingVolume box = TileBoundingVolume::fromBox(
        Vec3(1.0, 2.0, 3.0),
        Vec3(10.0, 0.0, 0.0),
        Vec3(0.0, 10.0, 0.0),
        Vec3(0.0, 0.0, 10.0));
    const TileBoundingVolume boxChild =
        ImplicitTileIdUtilities::computeBoundingVolume(box, tileID);
    EXPECT_EQ(TileBoundingVolumeKind::Box, boxChild.kind);
    EXPECT_EQ(Vec3(-4.0, 7.0, 8.0), boxChild.box.getCenter());
    EXPECT_EQ(Vec3(10.0, 10.0, 10.0), boxChild.box.getLengths());

    const TileBoundingVolume region =
        TileBoundingVolume::fromRegion(Rectangle(1.0, 2.0, 3.0, 4.0),
                                       10.0,
                                       20.0);
    const TileBoundingVolume regionChild =
        ImplicitTileIdUtilities::computeBoundingVolume(region, tileID);
    EXPECT_EQ(TileBoundingVolumeKind::Region, regionChild.kind);
    EXPECT_DOUBLE_EQ(1.0, regionChild.region.west());
    EXPECT_DOUBLE_EQ(3.0, regionChild.region.south());
    EXPECT_DOUBLE_EQ(2.0, regionChild.region.east());
    EXPECT_DOUBLE_EQ(4.0, regionChild.region.north());
    EXPECT_DOUBLE_EQ(15.0, regionChild.minimumHeight);
    EXPECT_DOUBLE_EQ(20.0, regionChild.maximumHeight);

    const TileBoundingVolume s2 = TileBoundingVolume::fromS2Cell(
        S2CellBoundingVolume(
            S2CellID::fromQuadtreeTileID(1, 0, 0, 0),
            10.0,
            20.0));
    const TileBoundingVolume s2Child =
        ImplicitTileIdUtilities::computeBoundingVolume(s2, tileID);
    EXPECT_EQ(TileBoundingVolumeKind::S2Cell, s2Child.kind);
    EXPECT_EQ(S2CellID::fromQuadtreeTileID(1, 1, 0, 1).getID(),
              s2Child.s2Cell.getCellID().getID());
    EXPECT_DOUBLE_EQ(15.0, s2Child.s2Cell.getMinimumHeight());
    EXPECT_DOUBLE_EQ(20.0, s2Child.s2Cell.getMaximumHeight());

    const TileBoundingVolume cylinder = TileBoundingVolume::fromCylinderRegion(
        BoundingCylinderRegion(
            Vec3(1.0, 2.0, 3.0),
            glm::dquat(Transforms::Z_UP_TO_Y_UP().raw()),
            2.0,
            glm::dvec2(0.0, 1.0)));
    const TileBoundingVolume cylinderChild =
        ImplicitTileIdUtilities::computeBoundingVolume(cylinder, tileID);
    EXPECT_EQ(TileBoundingVolumeKind::CylinderRegion, cylinderChild.kind);
    EXPECT_DOUBLE_EQ(1.0, cylinderChild.cylinderRegion.getHeight());
    EXPECT_EQ(glm::dvec2(0.0, 0.5),
              cylinderChild.cylinderRegion.getRadialBounds());
    EXPECT_EQ(glm::dvec2(0.0, MathUtils::OnePi),
              cylinderChild.cylinderRegion.getAngularBounds());
}

TEST(ImplicitTileIdUtilitiesTest,
     ComputeWholeCylinderQuadtreeBoundingVolumeMatchesCesiumNative) {
    const BoundingCylinderRegion root(
        Vec3(1.0, 2.0, 3.0),
        glm::dquat(Transforms::Z_UP_TO_Y_UP().raw()),
        2.0,
        glm::dvec2(0.0, 1.0));

    const BoundingCylinderRegion l1x0y0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            TileKey{"Geographic-TMS", 1, 0, 0});
    EXPECT_EQ(root.getHeight(), l1x0y0.getHeight());
    EXPECT_EQ(glm::dvec2(0.0, 0.5), l1x0y0.getRadialBounds());
    EXPECT_EQ(glm::dvec2(-MathUtils::OnePi, 0.0),
              l1x0y0.getAngularBounds());
    EXPECT_EQ(root.getRotation(), l1x0y0.getRotation());
    EXPECT_EQ(root.getTranslation(), l1x0y0.getTranslation());

    const BoundingCylinderRegion l1x1y0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            TileKey{"Geographic-TMS", 1, 1, 0});
    EXPECT_EQ(root.getHeight(), l1x1y0.getHeight());
    EXPECT_EQ(glm::dvec2(0.5, 1.0), l1x1y0.getRadialBounds());
    EXPECT_EQ(glm::dvec2(-MathUtils::OnePi, 0.0),
              l1x1y0.getAngularBounds());
    EXPECT_EQ(root.getRotation(), l1x1y0.getRotation());
    EXPECT_EQ(root.getTranslation(), l1x1y0.getTranslation());

    const BoundingCylinderRegion l1x0y1 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            TileKey{"Geographic-TMS", 1, 0, 1});
    EXPECT_EQ(root.getHeight(), l1x0y1.getHeight());
    EXPECT_EQ(glm::dvec2(0.0, 0.5), l1x0y1.getRadialBounds());
    EXPECT_EQ(glm::dvec2(0.0, MathUtils::OnePi),
              l1x0y1.getAngularBounds());
    EXPECT_EQ(root.getRotation(), l1x0y1.getRotation());
    EXPECT_EQ(root.getTranslation(), l1x0y1.getTranslation());
}

TEST(ImplicitTileIdUtilitiesTest,
     ComputeWholeCylinderOctreeBoundingVolumeMatchesCesiumNative) {
    const BoundingCylinderRegion root(
        Vec3(1.0, 2.0, 3.0),
        glm::dquat(Transforms::Z_UP_TO_Y_UP().raw()),
        2.0,
        glm::dvec2(0.0, 1.0));
    const double expectedHeight = 0.5 * root.getHeight();

    const BoundingCylinderRegion l1x0y0z0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 0, 0, 0});
    EXPECT_EQ(expectedHeight, l1x0y0z0.getHeight());
    EXPECT_EQ(glm::dvec2(0.0, 0.5), l1x0y0z0.getRadialBounds());
    EXPECT_EQ(glm::dvec2(-MathUtils::OnePi, 0.0),
              l1x0y0z0.getAngularBounds());
    EXPECT_EQ(root.getRotation(), l1x0y0z0.getRotation());
    EXPECT_EQ(root.getTranslation() + Vec3(0.0, -0.5 * expectedHeight, 0.0),
              l1x0y0z0.getTranslation());

    const BoundingCylinderRegion l1x1y0z0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 1, 0, 0});
    EXPECT_EQ(expectedHeight, l1x1y0z0.getHeight());
    EXPECT_EQ(glm::dvec2(0.5, 1.0), l1x1y0z0.getRadialBounds());
    EXPECT_EQ(glm::dvec2(-MathUtils::OnePi, 0.0),
              l1x1y0z0.getAngularBounds());
    EXPECT_EQ(root.getRotation(), l1x1y0z0.getRotation());
    EXPECT_EQ(root.getTranslation() + Vec3(0.0, -0.5 * expectedHeight, 0.0),
              l1x1y0z0.getTranslation());

    const BoundingCylinderRegion l1x0y1z0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 0, 1, 0});
    EXPECT_EQ(expectedHeight, l1x0y1z0.getHeight());
    EXPECT_EQ(glm::dvec2(0.0, 0.5), l1x0y1z0.getRadialBounds());
    EXPECT_EQ(glm::dvec2(0.0, MathUtils::OnePi),
              l1x0y1z0.getAngularBounds());
    EXPECT_EQ(root.getRotation(), l1x0y1z0.getRotation());
    EXPECT_EQ(root.getTranslation() + Vec3(0.0, -0.5 * expectedHeight, 0.0),
              l1x0y1z0.getTranslation());

    const BoundingCylinderRegion l1x0y0z1 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 0, 0, 1});
    EXPECT_EQ(expectedHeight, l1x0y0z1.getHeight());
    EXPECT_EQ(glm::dvec2(0.0, 0.5), l1x0y0z1.getRadialBounds());
    EXPECT_EQ(glm::dvec2(-MathUtils::OnePi, 0.0),
              l1x0y0z1.getAngularBounds());
    EXPECT_EQ(root.getRotation(), l1x0y0z1.getRotation());
    EXPECT_EQ(root.getTranslation() + Vec3(0.0, 0.5 * expectedHeight, 0.0),
              l1x0y0z1.getTranslation());
}

TEST(ImplicitTileIdUtilitiesTest,
     ComputePartialCylinderQuadtreeBoundingVolumeMatchesCesiumNative) {
    const BoundingCylinderRegion root(
        Vec3(-1.0, 1.0, 2.0),
        glm::dquat(1.0, 0.0, 0.0, 0.0),
        2.0,
        glm::dvec2(0.0, 1.0),
        glm::dvec2(-MathUtils::PiOverTwo, MathUtils::PiOverTwo));

    const BoundingCylinderRegion l1x0y0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            TileKey{"Geographic-TMS", 1, 0, 0});
    EXPECT_EQ(root.getHeight(), l1x0y0.getHeight());
    EXPECT_EQ(glm::dvec2(0.0, 0.5), l1x0y0.getRadialBounds());
    expectDVec2Near(l1x0y0.getAngularBounds(),
                    glm::dvec2(-MathUtils::PiOverTwo, 0.0));
    EXPECT_EQ(root.getRotation(), l1x0y0.getRotation());
    EXPECT_EQ(root.getTranslation(), l1x0y0.getTranslation());

    const BoundingCylinderRegion l1x1y0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            TileKey{"Geographic-TMS", 1, 1, 0});
    EXPECT_EQ(root.getHeight(), l1x1y0.getHeight());
    EXPECT_EQ(glm::dvec2(0.5, 1.0), l1x1y0.getRadialBounds());
    expectDVec2Near(l1x1y0.getAngularBounds(),
                    glm::dvec2(-MathUtils::PiOverTwo, 0.0));
    EXPECT_EQ(root.getRotation(), l1x1y0.getRotation());
    EXPECT_EQ(root.getTranslation(), l1x1y0.getTranslation());

    const BoundingCylinderRegion l1x0y1 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            TileKey{"Geographic-TMS", 1, 0, 1});
    EXPECT_EQ(root.getHeight(), l1x0y1.getHeight());
    EXPECT_EQ(glm::dvec2(0.0, 0.5), l1x0y1.getRadialBounds());
    expectDVec2Near(l1x0y1.getAngularBounds(),
                    glm::dvec2(0.0, MathUtils::PiOverTwo));
    EXPECT_EQ(root.getRotation(), l1x0y1.getRotation());
    EXPECT_EQ(root.getTranslation(), l1x0y1.getTranslation());
}

TEST(ImplicitTileIdUtilitiesTest,
     ComputePartialCylinderOctreeBoundingVolumeMatchesCesiumNative) {
    const BoundingCylinderRegion root(
        Vec3(-1.0, 1.0, 2.0),
        glm::dquat(1.0, 0.0, 0.0, 0.0),
        2.0,
        glm::dvec2(0.0, 1.0),
        glm::dvec2(-MathUtils::PiOverTwo, MathUtils::PiOverTwo));
    const double expectedHeight = 0.5 * root.getHeight();

    const BoundingCylinderRegion l1x0y0z0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 0, 0, 0});
    EXPECT_EQ(expectedHeight, l1x0y0z0.getHeight());
    EXPECT_EQ(glm::dvec2(0.0, 0.5), l1x0y0z0.getRadialBounds());
    expectDVec2Near(l1x0y0z0.getAngularBounds(),
                    glm::dvec2(-MathUtils::PiOverTwo, 0.0));
    EXPECT_EQ(root.getRotation(), l1x0y0z0.getRotation());
    EXPECT_EQ(root.getTranslation() + Vec3(0.0, 0.0, -0.5 * expectedHeight),
              l1x0y0z0.getTranslation());

    const BoundingCylinderRegion l1x1y0z0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 1, 0, 0});
    EXPECT_EQ(expectedHeight, l1x1y0z0.getHeight());
    EXPECT_EQ(glm::dvec2(0.5, 1.0), l1x1y0z0.getRadialBounds());
    expectDVec2Near(l1x1y0z0.getAngularBounds(),
                    glm::dvec2(-MathUtils::PiOverTwo, 0.0));
    EXPECT_EQ(root.getRotation(), l1x1y0z0.getRotation());
    EXPECT_EQ(root.getTranslation() + Vec3(0.0, 0.0, -0.5 * expectedHeight),
              l1x1y0z0.getTranslation());

    const BoundingCylinderRegion l1x0y1z0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 0, 1, 0});
    EXPECT_EQ(expectedHeight, l1x0y1z0.getHeight());
    EXPECT_EQ(glm::dvec2(0.0, 0.5), l1x0y1z0.getRadialBounds());
    expectDVec2Near(l1x0y1z0.getAngularBounds(),
                    glm::dvec2(0.0, MathUtils::PiOverTwo));
    EXPECT_EQ(root.getRotation(), l1x0y1z0.getRotation());
    EXPECT_EQ(root.getTranslation() + Vec3(0.0, 0.0, -0.5 * expectedHeight),
              l1x0y1z0.getTranslation());

    const BoundingCylinderRegion l1x0y0z1 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 0, 0, 1});
    EXPECT_EQ(expectedHeight, l1x0y0z1.getHeight());
    EXPECT_EQ(glm::dvec2(0.0, 0.5), l1x0y0z1.getRadialBounds());
    expectDVec2Near(l1x0y0z1.getAngularBounds(),
                    glm::dvec2(-MathUtils::PiOverTwo, 0.0));
    EXPECT_EQ(root.getRotation(), l1x0y0z1.getRotation());
    EXPECT_EQ(root.getTranslation() + Vec3(0.0, 0.0, 0.5 * expectedHeight),
              l1x0y0z1.getTranslation());
}

TEST(ImplicitTileIdUtilitiesTest,
     ComputeDiscontinuousCylinderQuadtreeBoundingVolumeMatchesCesiumNative) {
    const BoundingCylinderRegion root(
        Vec3(-1.0, 1.0, 2.0),
        glm::dquat(1.0, 0.0, 0.0, 0.0),
        2.0,
        glm::dvec2(0.0, 1.0),
        glm::dvec2(MathUtils::PiOverFour, -MathUtils::PiOverFour));

    const BoundingCylinderRegion l1x0y0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            TileKey{"Geographic-TMS", 1, 0, 0});
    EXPECT_EQ(root.getHeight(), l1x0y0.getHeight());
    EXPECT_EQ(glm::dvec2(0.0, 0.5), l1x0y0.getRadialBounds());
    expectDVec2Near(l1x0y0.getAngularBounds(),
                    glm::dvec2(MathUtils::PiOverFour, MathUtils::OnePi));
    EXPECT_EQ(root.getRotation(), l1x0y0.getRotation());
    EXPECT_EQ(root.getTranslation(), l1x0y0.getTranslation());

    const BoundingCylinderRegion l1x1y0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            TileKey{"Geographic-TMS", 1, 1, 0});
    EXPECT_EQ(root.getHeight(), l1x1y0.getHeight());
    EXPECT_EQ(glm::dvec2(0.5, 1.0), l1x1y0.getRadialBounds());
    expectDVec2Near(l1x1y0.getAngularBounds(),
                    glm::dvec2(MathUtils::PiOverFour, MathUtils::OnePi));
    EXPECT_EQ(root.getRotation(), l1x1y0.getRotation());
    EXPECT_EQ(root.getTranslation(), l1x1y0.getTranslation());

    const BoundingCylinderRegion l1x0y1 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            TileKey{"Geographic-TMS", 1, 0, 1});
    EXPECT_EQ(root.getHeight(), l1x0y1.getHeight());
    EXPECT_EQ(glm::dvec2(0.0, 0.5), l1x0y1.getRadialBounds());
    expectDVec2Near(l1x0y1.getAngularBounds(),
                    glm::dvec2(-MathUtils::OnePi, -MathUtils::PiOverFour));
    EXPECT_EQ(root.getRotation(), l1x0y1.getRotation());
    EXPECT_EQ(root.getTranslation(), l1x0y1.getTranslation());
}

TEST(ImplicitTileIdUtilitiesTest,
     ComputeDiscontinuousCylinderOctreeBoundingVolumeMatchesCesiumNative) {
    const BoundingCylinderRegion root(
        Vec3(-1.0, 1.0, 2.0),
        glm::dquat(1.0, 0.0, 0.0, 0.0),
        2.0,
        glm::dvec2(0.0, 1.0),
        glm::dvec2(MathUtils::PiOverFour, -MathUtils::PiOverFour));
    const double expectedHeight = 0.5 * root.getHeight();

    const BoundingCylinderRegion l1x0y0z0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 0, 0, 0});
    EXPECT_EQ(expectedHeight, l1x0y0z0.getHeight());
    EXPECT_EQ(glm::dvec2(0.0, 0.5), l1x0y0z0.getRadialBounds());
    expectDVec2Near(l1x0y0z0.getAngularBounds(),
                    glm::dvec2(MathUtils::PiOverFour, MathUtils::OnePi));
    EXPECT_EQ(root.getRotation(), l1x0y0z0.getRotation());
    EXPECT_EQ(root.getTranslation() + Vec3(0.0, 0.0, -0.5 * expectedHeight),
              l1x0y0z0.getTranslation());

    const BoundingCylinderRegion l1x1y0z0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 1, 0, 0});
    EXPECT_EQ(expectedHeight, l1x1y0z0.getHeight());
    EXPECT_EQ(glm::dvec2(0.5, 1.0), l1x1y0z0.getRadialBounds());
    expectDVec2Near(l1x1y0z0.getAngularBounds(),
                    glm::dvec2(MathUtils::PiOverFour, MathUtils::OnePi));
    EXPECT_EQ(root.getRotation(), l1x1y0z0.getRotation());
    EXPECT_EQ(root.getTranslation() + Vec3(0.0, 0.0, -0.5 * expectedHeight),
              l1x1y0z0.getTranslation());

    const BoundingCylinderRegion l1x0y1z0 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 0, 1, 0});
    EXPECT_EQ(expectedHeight, l1x0y1z0.getHeight());
    EXPECT_EQ(glm::dvec2(0.0, 0.5), l1x0y1z0.getRadialBounds());
    expectDVec2Near(l1x0y1z0.getAngularBounds(),
                    glm::dvec2(-MathUtils::OnePi, -MathUtils::PiOverFour));
    EXPECT_EQ(root.getRotation(), l1x0y1z0.getRotation());
    EXPECT_EQ(root.getTranslation() + Vec3(0.0, 0.0, -0.5 * expectedHeight),
              l1x0y1z0.getTranslation());

    const BoundingCylinderRegion l1x0y0z1 =
        ImplicitTileIdUtilities::computeBoundingVolume(
            root,
            OctreeTileID{1, 0, 0, 1});
    EXPECT_EQ(expectedHeight, l1x0y0z1.getHeight());
    EXPECT_EQ(glm::dvec2(0.0, 0.5), l1x0y0z1.getRadialBounds());
    expectDVec2Near(l1x0y0z1.getAngularBounds(),
                    glm::dvec2(MathUtils::PiOverFour, MathUtils::OnePi));
    EXPECT_EQ(root.getRotation(), l1x0y0z1.getRotation());
    EXPECT_EQ(root.getTranslation() + Vec3(0.0, 0.0, 0.5 * expectedHeight),
              l1x0y0z1.getTranslation());
}

TEST(ImplicitTileIdUtilitiesTest, ParentIdMatchesCesiumNativeOptionalSemantics) {
    const std::optional<TileKey> quadtreeParent =
        ImplicitTileIdUtilities::parentId(TileKey{"Geographic-TMS", 2, 1, 2});
    ASSERT_TRUE(quadtreeParent.has_value());
    EXPECT_EQ((TileKey{"Geographic-TMS", 1, 0, 1}), *quadtreeParent);
    const std::optional<TileKey> quadtreeGrandparent =
        ImplicitTileIdUtilities::parentId(*quadtreeParent);
    ASSERT_TRUE(quadtreeGrandparent.has_value());
    EXPECT_EQ((TileKey{"Geographic-TMS", 0, 0, 0}), *quadtreeGrandparent);
    EXPECT_FALSE(
        ImplicitTileIdUtilities::parentId(
            TileKey{"Geographic-TMS", 0, 0, 0})
            .has_value());

    const std::optional<OctreeTileID> octreeParent =
        ImplicitTileIdUtilities::parentId(OctreeTileID{2, 3, 1, 2});
    ASSERT_TRUE(octreeParent.has_value());
    EXPECT_EQ((OctreeTileID{1, 1, 0, 1}), *octreeParent);
    const std::optional<OctreeTileID> octreeGrandparent =
        ImplicitTileIdUtilities::parentId(*octreeParent);
    ASSERT_TRUE(octreeGrandparent.has_value());
    EXPECT_EQ((OctreeTileID{0, 0, 0, 0}), *octreeGrandparent);
    EXPECT_FALSE(
        ImplicitTileIdUtilities::parentId(OctreeTileID{0, 0, 0, 0})
            .has_value());
}

TEST(ImplicitTileIdUtilitiesTest, SubtreeRootIdMatchesCesiumNative) {
    EXPECT_EQ((TileKey{"Geographic-TMS", 10, 2, 3}),
              ImplicitTileIdUtilities::subtreeRootId(
                  5,
                  TileKey{"Geographic-TMS", 10, 2, 3}));
    EXPECT_EQ((TileKey{"Geographic-TMS", 8, 0, 0}),
              ImplicitTileIdUtilities::subtreeRootId(
                  4,
                  TileKey{"Geographic-TMS", 10, 2, 3}));

    EXPECT_EQ((OctreeTileID{10, 2, 3, 4}),
              ImplicitTileIdUtilities::subtreeRootId(
                  5,
                  OctreeTileID{10, 2, 3, 4}));
    EXPECT_EQ((OctreeTileID{8, 0, 0, 1}),
              ImplicitTileIdUtilities::subtreeRootId(
                  4,
                  OctreeTileID{10, 2, 3, 4}));
}

TEST(ImplicitTileIdUtilitiesTest, AbsoluteTileIdToRelativeMatchesCesiumNative) {
    EXPECT_EQ((TileKey{"Geographic-TMS", 11, 2, 3}),
              ImplicitTileIdUtilities::absoluteTileIdToRelative(
                  TileKey{"Geographic-TMS", 0, 0, 0},
                  TileKey{"Geographic-TMS", 11, 2, 3}));
    EXPECT_EQ((TileKey{"Geographic-TMS", 0, 0, 0}),
              ImplicitTileIdUtilities::absoluteTileIdToRelative(
                  TileKey{"Geographic-TMS", 11, 2, 3},
                  TileKey{"Geographic-TMS", 11, 2, 3}));
    EXPECT_EQ((TileKey{"Geographic-TMS", 1, 1, 1}),
              ImplicitTileIdUtilities::absoluteTileIdToRelative(
                  TileKey{"Geographic-TMS", 11, 2, 3},
                  TileKey{"Geographic-TMS", 12, 5, 7}));

    EXPECT_EQ((OctreeTileID{11, 2, 3, 4}),
              ImplicitTileIdUtilities::absoluteTileIdToRelative(
                  OctreeTileID{0, 0, 0, 0},
                  OctreeTileID{11, 2, 3, 4}));
    EXPECT_EQ((OctreeTileID{0, 0, 0, 0}),
              ImplicitTileIdUtilities::absoluteTileIdToRelative(
                  OctreeTileID{11, 2, 3, 4},
                  OctreeTileID{11, 2, 3, 4}));
    EXPECT_EQ((OctreeTileID{1, 1, 1, 1}),
              ImplicitTileIdUtilities::absoluteTileIdToRelative(
                  OctreeTileID{11, 2, 3, 4},
                  OctreeTileID{12, 5, 7, 9}));
}

TEST(ImplicitTileIdUtilitiesTest, MortonIndexMatchesCesiumNativeExamples) {
    EXPECT_EQ(14ULL,
              ImplicitTileIdUtilities::mortonIndex(
                  TileKey{"Geographic-TMS", 11, 2, 3}));
    EXPECT_EQ(282ULL,
              ImplicitTileIdUtilities::mortonIndex(
                  OctreeTileID{11, 2, 3, 4}));

    EXPECT_EQ(1ULL,
              ImplicitTileIdUtilities::relativeMortonIndex(
                  TileKey{"Geographic-TMS", 11, 2, 3},
                  TileKey{"Geographic-TMS", 12, 5, 6}));
    EXPECT_EQ(1ULL,
              ImplicitTileIdUtilities::relativeMortonIndex(
                  OctreeTileID{11, 2, 3, 4},
                  OctreeTileID{12, 5, 6, 8}));
}

TEST(ImplicitTileIdUtilitiesTest,
     RelativeMortonIndexMatchesCesiumNativeForDeepNonzeroSubtreeRoots) {
    const TileKey quadtreeRoot{"Geographic-TMS", 4, 3, 5};
    const TileKey quadtreeDescendant{"Geographic-TMS", 7, 28, 43};
    EXPECT_EQ((TileKey{"Geographic-TMS", 3, 4, 3}),
              ImplicitTileIdUtilities::absoluteTileIdToRelative(
                  quadtreeRoot,
                  quadtreeDescendant));
    EXPECT_EQ(26ULL,
              ImplicitTileIdUtilities::relativeMortonIndex(
                  quadtreeRoot,
                  quadtreeDescendant));

    const OctreeTileID octreeRoot{4, 3, 5, 2};
    const OctreeTileID octreeDescendant{7, 28, 43, 21};
    EXPECT_EQ((OctreeTileID{3, 4, 3, 5}),
              ImplicitTileIdUtilities::absoluteTileIdToRelative(
                  octreeRoot,
                  octreeDescendant));
    EXPECT_EQ(342ULL,
              ImplicitTileIdUtilities::relativeMortonIndex(
                  octreeRoot,
                  octreeDescendant));
}

TEST(ImplicitTileIdUtilitiesTest, LevelDenominatorMatchesCesiumNative) {
    EXPECT_DOUBLE_EQ(1.0, ImplicitTileIdUtilities::levelDenominator(0));
    EXPECT_DOUBLE_EQ(2.0, ImplicitTileIdUtilities::levelDenominator(1));
    EXPECT_DOUBLE_EQ(4.0, ImplicitTileIdUtilities::levelDenominator(2));
    EXPECT_DOUBLE_EQ(4294967296.0,
                     ImplicitTileIdUtilities::levelDenominator(32));
    EXPECT_DOUBLE_EQ(1099511627776.0,
                     ImplicitTileIdUtilities::levelDenominator(40));
}
