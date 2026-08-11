#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/BoundingRegionBuilder.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/GeographicProjection.h"
#include "earth_engine/core/geodesy/Gcj02CoordinateTransform.h"
#include "earth_engine/core/geodesy/Projection.h"
#include "earth_engine/core/geodesy/WebMercatorProjection.h"
#include "earth_engine/core/math/AxisAlignedBox.h"
#include "earth_engine/core/math/MathUtils.h"
#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/tiling/RasterOverlayProjection.h"

#include <array>
#include <cmath>
#include <glm/ext/vector_double2.hpp>
#include <limits>

using namespace earth_engine;

namespace {

Rectangle xyzTileRectangle(int level, int x, int y) {
    const double tileCount =
        static_cast<double>(int64_t{1} << level);
    const double west =
        MathUtils::TwoPi * static_cast<double>(x) / tileCount -
        MathUtils::OnePi;
    const double east =
        MathUtils::TwoPi * static_cast<double>(x + 1) / tileCount -
        MathUtils::OnePi;
    const double north = std::atan(std::sinh(
        MathUtils::OnePi *
        (1.0 - 2.0 * static_cast<double>(y) / tileCount)));
    const double south = std::atan(std::sinh(
        MathUtils::OnePi *
        (1.0 - 2.0 * static_cast<double>(y + 1) / tileCount)));
    return Rectangle(west, south, east, north);
}

void expectGcjBoundsContainGrid(const Rectangle& worldBounds,
                                int longitudeSamples,
                                int latitudeSamples) {
    const Rectangle sourceBounds =
        worldRectangleToRasterSource(
            worldBounds,
            RasterOverlayProjection::Gcj02WebMercator);
    const Rectangle projectedBounds =
        projectWorldRectangleForRasterOverlay(
            worldBounds,
            RasterOverlayProjection::Gcj02WebMercator);
    ASSERT_FALSE(sourceBounds.isEmpty());
    ASSERT_FALSE(projectedBounds.isEmpty());

    for (int y = 0; y < latitudeSamples; ++y) {
        const double latitudeT = latitudeSamples == 1
            ? 0.0
            : static_cast<double>(y) /
                  static_cast<double>(latitudeSamples - 1);
        const double latitude =
            worldBounds.south() +
            worldBounds.height() * latitudeT;
        for (int x = 0; x < longitudeSamples; ++x) {
            const double longitudeT = longitudeSamples == 1
                ? 0.0
                : static_cast<double>(x) /
                      static_cast<double>(longitudeSamples - 1);
            double longitude =
                worldBounds.west() +
                worldBounds.width() * longitudeT;
            if (longitude > MathUtils::OnePi) {
                longitude -= MathUtils::TwoPi;
            }
            const Cartographic world =
                Cartographic::fromRadians(longitude, latitude);
            const Cartographic source =
                Gcj02CoordinateTransform::fromWgs84(world);
            const Vec3 projected =
                projectWorldPositionForRasterOverlay(
                    world,
                    RasterOverlayProjection::Gcj02WebMercator);

            EXPECT_TRUE(sourceBounds.contains(
                source.longitude(),
                source.latitude()));
            EXPECT_TRUE(projectedBounds.contains(
                projected.x(),
                projected.y()));
        }
    }
}

} // namespace

TEST(Gcj02CoordinateTransformTest, KnownBeijingControlPointMatchesGcj02) {
    const Cartographic wgs84 =
        Cartographic::fromDegrees(116.397389, 39.908722, 88.0);

    const Cartographic gcj02 =
        Gcj02CoordinateTransform::fromWgs84(wgs84);

    EXPECT_NEAR(116.40363255334069, gcj02.longitudeDegrees(), 1e-10);
    EXPECT_NEAR(39.91012547567846, gcj02.latitudeDegrees(), 1e-10);
    EXPECT_DOUBLE_EQ(88.0, gcj02.height());
}

TEST(Gcj02CoordinateTransformTest, OutsideChinaRemainsWgs84) {
    const Cartographic pennsylvania =
        Cartographic::fromDegrees(-75.612094, 40.042531, 25.0);

    const Cartographic transformed =
        Gcj02CoordinateTransform::fromWgs84(pennsylvania);

    EXPECT_EQ(pennsylvania, transformed);
}

// crossesChinaBounds 判的是「矩形内同时存在被变换和不被变换的点」——即 δ 在矩形
// 内是阶跃而非梯度。它是 TerrainPageStore 放弃视锥剔除的闸,判 false 就等于宣称
// 单个 per-tile worldOffset 能代表整片,漏判会让那些 cell 在合批后渲成纯白面。
TEST(Gcj02CoordinateTransformTest, WhollyInsideChinaRectangleDoesNotCross) {
    EXPECT_FALSE(Gcj02CoordinateTransform::crossesChinaBounds(
        Rectangle::fromDegrees(106.0, 29.0, 107.0, 30.0)));
}

TEST(Gcj02CoordinateTransformTest, WhollyOutsideChinaRectangleDoesNotCross) {
    EXPECT_FALSE(Gcj02CoordinateTransform::crossesChinaBounds(
        Rectangle::fromDegrees(-76.0, 40.0, -75.0, 41.0)));
    // 纬度带对上、经度带没对上(印度洋)也不算跨。
    EXPECT_FALSE(Gcj02CoordinateTransform::crossesChinaBounds(
        Rectangle::fromDegrees(60.0, 20.0, 70.0, 30.0)));
}

TEST(Gcj02CoordinateTransformTest, RectangleStraddlingEachChinaEdgeCrosses) {
    // 东 137.8347° / 西 72.004° / 南 0.8293° / 北 55.8271°,逐边各跨一次。
    EXPECT_TRUE(Gcj02CoordinateTransform::crossesChinaBounds(
        Rectangle::fromDegrees(137.0, 45.0, 139.0, 46.0)));
    EXPECT_TRUE(Gcj02CoordinateTransform::crossesChinaBounds(
        Rectangle::fromDegrees(71.0, 38.0, 73.0, 39.0)));
    EXPECT_TRUE(Gcj02CoordinateTransform::crossesChinaBounds(
        Rectangle::fromDegrees(110.0, 0.0, 111.0, 2.0)));
    EXPECT_TRUE(Gcj02CoordinateTransform::crossesChinaBounds(
        Rectangle::fromDegrees(110.0, 55.0, 111.0, 57.0)));
}

TEST(Gcj02CoordinateTransformTest, RectangleEnclosingChinaBoundsCrosses) {
    // 低 zoom 的粗瓦片把整个框吃进去:内部仍有阶跃,必须判跨。
    EXPECT_TRUE(Gcj02CoordinateTransform::crossesChinaBounds(
        Rectangle::fromDegrees(-180.0, -85.0, 180.0, 85.0)));
}

TEST(Gcj02CoordinateTransformTest, EmptyRectangleDoesNotCross) {
    EXPECT_FALSE(
        Gcj02CoordinateTransform::crossesChinaBounds(Rectangle::EMPTY));
}

TEST(Gcj02CoordinateTransformTest,
     OutsideChinaRectangleMatchesStandardWebMercatorExactly) {
    const Rectangle worldBounds =
        xyzTileRectangle(18, 76012, 99201);

    EXPECT_EQ(
        worldBounds,
        worldRectangleToRasterSource(
            worldBounds,
            RasterOverlayProjection::Gcj02WebMercator));
    EXPECT_EQ(
        projectWorldRectangleForRasterOverlay(
            worldBounds,
            RasterOverlayProjection::WebMercator),
        projectWorldRectangleForRasterOverlay(
            worldBounds,
            RasterOverlayProjection::Gcj02WebMercator));
}

TEST(Gcj02CoordinateTransformTest,
     OutsideChinaAntimeridianRectangleRemainsBitExact) {
    const Rectangle worldBounds =
        Rectangle::fromDegrees(170.0, -10.0, -170.0, 10.0);

    EXPECT_EQ(
        worldBounds,
        worldRectangleToRasterSource(
            worldBounds,
            RasterOverlayProjection::Gcj02WebMercator));
    EXPECT_EQ(
        projectWorldRectangleForRasterOverlay(
            worldBounds,
            RasterOverlayProjection::WebMercator),
        projectWorldRectangleForRasterOverlay(
            worldBounds,
            RasterOverlayProjection::Gcj02WebMercator));
}

TEST(Gcj02CoordinateTransformTest,
     AntimeridianRectangleKeepsWrappedConservativeBounds) {
    const Rectangle worldBounds =
        Rectangle::fromDegrees(100.0, 20.0, -170.0, 40.0);
    const Rectangle sourceBounds =
        worldRectangleToRasterSource(
            worldBounds,
            RasterOverlayProjection::Gcj02WebMercator);

    EXPECT_TRUE(sourceBounds.crossesAntimeridian());
    EXPECT_LT(sourceBounds.width(), MathUtils::OnePi);
    expectGcjBoundsContainGrid(worldBounds, 257, 65);
}

TEST(Gcj02CoordinateTransformTest,
     ConservativeBoundsContainKnownZ4NorthEdgeMiss) {
    const Rectangle worldBounds =
        xyzTileRectangle(4, 14, 5);
    const Rectangle sourceBounds =
        worldRectangleToRasterSource(
            worldBounds,
            RasterOverlayProjection::Gcj02WebMercator);
    const Rectangle projectedBounds =
        projectWorldRectangleForRasterOverlay(
            worldBounds,
            RasterOverlayProjection::Gcj02WebMercator);

    for (int i = 0; i <= 4096; ++i) {
        const double longitude =
            worldBounds.west() +
            worldBounds.width() *
                static_cast<double>(i) / 4096.0;
        const Cartographic world = Cartographic::fromRadians(
            longitude,
            worldBounds.north());
        const Cartographic source =
            Gcj02CoordinateTransform::fromWgs84(world);
        const Vec3 projected =
            projectWorldPositionForRasterOverlay(
                world,
                RasterOverlayProjection::Gcj02WebMercator);
        EXPECT_LE(source.latitude(), sourceBounds.north());
        EXPECT_LE(projected.y(), projectedBounds.north());
    }
}

TEST(Gcj02CoordinateTransformTest,
     ConservativeBoundsContainBroadInteriorRectangle) {
    expectGcjBoundsContainGrid(
        Rectangle::fromDegrees(100.0, 20.0, 120.0, 40.0),
        257,
        257);
}

TEST(Gcj02CoordinateTransformTest,
     ConservativeBoundsContainRepresentativeXyzTiles) {
    const std::array<std::array<int, 3>, 8> keys{{
        {3, 7, 3},
        {4, 14, 5},
        {5, 24, 9},
        {6, 44, 20},
        {7, 89, 41},
        {8, 205, 80},
        {9, 426, 160},
        {10, 770, 319}}};
    for (const auto& key : keys) {
        expectGcjBoundsContainGrid(
            xyzTileRectangle(key[0], key[1], key[2]),
            65,
            65);
    }
}

TEST(Gcj02CoordinateTransformTest,
     ProjectedRectangleIncludesGcjBoundaryDiscontinuities) {
    const std::array<Rectangle, 4> crossings{
        Rectangle::fromDegrees(71.95, 20.0, 72.05, 20.1),
        Rectangle::fromDegrees(137.80, 20.0, 137.90, 20.1),
        Rectangle::fromDegrees(100.0, 0.78, 100.1, 0.88),
        Rectangle::fromDegrees(100.0, 55.78, 100.1, 55.88)};
    for (const Rectangle& crossing : crossings) {
        expectGcjBoundsContainGrid(crossing, 129, 129);
    }
}

TEST(GeographicProjectionTest, ProjectUsesCesiumNativeLinearRadiansScale) {
    const GeographicProjection projection(Ellipsoid::WGS84());
    const Cartographic input =
        Cartographic::fromRadians(1.0, 0.5, 1234.5);

    const Vec3 projected = projection.project(input);

    EXPECT_DOUBLE_EQ(Ellipsoid::WGS84().maximumRadius(), projected.x());
    EXPECT_DOUBLE_EQ(Ellipsoid::WGS84().maximumRadius() * 0.5, projected.y());
    EXPECT_DOUBLE_EQ(1234.5, projected.z());
}

TEST(GeographicProjectionTest, ProjectUsesEllipsoidMaximumRadiusLikeCesiumNative) {
    // Source-derived from cesium-native GeographicProjection constructor:
    // projected x/y scale is ellipsoid.getMaximumRadius().
    const Ellipsoid ellipsoid(2.0, 5.0, 3.0);
    const GeographicProjection projection(ellipsoid);

    const Vec3 projected =
        projection.project(Cartographic::fromRadians(1.0, -0.5, 7.0));

    EXPECT_DOUBLE_EQ(5.0, projected.x());
    EXPECT_DOUBLE_EQ(-2.5, projected.y());
    EXPECT_DOUBLE_EQ(7.0, projected.z());
}

TEST(GeographicProjectionTest, UnprojectPreservesHeight) {
    const GeographicProjection projection(Ellipsoid::WGS84());
    const Vec3 projected(Ellipsoid::WGS84().maximumRadius(),
                         Ellipsoid::WGS84().maximumRadius() * -0.25,
                         987.0);

    const Cartographic cartographic = projection.unproject(projected);

    EXPECT_DOUBLE_EQ(1.0, cartographic.longitude());
    EXPECT_DOUBLE_EQ(-0.25, cartographic.latitude());
    EXPECT_DOUBLE_EQ(987.0, cartographic.height());
}

TEST(GeographicProjectionTest, Unproject2DSetsHeightToZeroLikeCesiumNative) {
    const GeographicProjection projection(Ellipsoid::WGS84());
    const glm::dvec2 projected(Ellipsoid::WGS84().maximumRadius() * 0.75,
                               Ellipsoid::WGS84().maximumRadius() * -0.25);

    const Cartographic cartographic = projection.unproject(projected);

    EXPECT_DOUBLE_EQ(0.75, cartographic.longitude());
    EXPECT_DOUBLE_EQ(-0.25, cartographic.latitude());
    EXPECT_DOUBLE_EQ(0.0, cartographic.height());
}

TEST(GeographicProjectionTest, ProjectAndUnprojectRectangleMatchesCesiumNative) {
    const GeographicProjection projection(Ellipsoid::WGS84());
    const Rectangle globeRectangle =
        Rectangle::fromDegrees(-120.0, -40.0, -60.0, 30.0);

    const Rectangle projected = projection.project(globeRectangle);
    EXPECT_NEAR(globeRectangle.west() * Ellipsoid::WGS84().maximumRadius(),
                projected.west(),
                1e-9);
    EXPECT_NEAR(globeRectangle.south() * Ellipsoid::WGS84().maximumRadius(),
                projected.south(),
                1e-9);
    EXPECT_NEAR(globeRectangle.east() * Ellipsoid::WGS84().maximumRadius(),
                projected.east(),
                1e-9);
    EXPECT_NEAR(globeRectangle.north() * Ellipsoid::WGS84().maximumRadius(),
                projected.north(),
                1e-9);

    const Rectangle unprojected = projection.unproject(projected);
    EXPECT_NEAR(globeRectangle.west(), unprojected.west(), 1e-15);
    EXPECT_NEAR(globeRectangle.south(), unprojected.south(), 1e-15);
    EXPECT_NEAR(globeRectangle.east(), unprojected.east(), 1e-15);
    EXPECT_NEAR(globeRectangle.north(), unprojected.north(), 1e-15);
}

TEST(GeographicProjectionTest, MaximumProjectedRectangleMatchesCesiumNative) {
    const Rectangle maximum =
        GeographicProjection::computeMaximumProjectedRectangle(Ellipsoid::WGS84());

    EXPECT_NEAR(-M_PI * Ellipsoid::WGS84().maximumRadius(),
                maximum.west(),
                1e-9);
    EXPECT_NEAR(-M_PI * 0.5 * Ellipsoid::WGS84().maximumRadius(),
                maximum.south(),
                1e-9);
    EXPECT_NEAR(M_PI * Ellipsoid::WGS84().maximumRadius(),
                maximum.east(),
                1e-9);
    EXPECT_NEAR(M_PI * 0.5 * Ellipsoid::WGS84().maximumRadius(),
                maximum.north(),
                1e-9);
}

TEST(GeographicProjectionTest, MaximumGlobeRectangleMatchesCesiumNative) {
    const Rectangle maximum = GeographicProjection::maximumGlobeRectangle();

    EXPECT_DOUBLE_EQ(-M_PI, maximum.west());
    EXPECT_DOUBLE_EQ(-M_PI * 0.5, maximum.south());
    EXPECT_DOUBLE_EQ(M_PI, maximum.east());
    EXPECT_DOUBLE_EQ(M_PI * 0.5, maximum.north());
}

TEST(GeographicProjectionTest, EqualityUsesCesiumNativeEllipsoidSemantics) {
    const GeographicProjection lhs(Ellipsoid::WGS84());
    const GeographicProjection same(
        Ellipsoid(6378137.0, 6378137.0, 6356752.3142451793));
    const GeographicProjection different(Ellipsoid::UNIT_SPHERE());

    EXPECT_TRUE(lhs == same);
    EXPECT_FALSE(lhs != same);
    EXPECT_FALSE(lhs == different);
    EXPECT_TRUE(lhs != different);
}

TEST(WebMercatorProjectionTest, MaximumLatitudeMatchesCesiumNative) {
    EXPECT_NEAR(1.4844222297453324,
                WebMercatorProjection::maximumLatitude(),
                1e-15);
    EXPECT_NEAR(M_PI,
                WebMercatorProjection::geodeticLatitudeToMercatorAngle(
                    WebMercatorProjection::maximumLatitude()),
                1e-14);
}

TEST(WebMercatorProjectionTest, MercatorAngleRoundtripsLatitude) {
    const double latitude = 40.0 * M_PI / 180.0;

    const double mercator =
        WebMercatorProjection::geodeticLatitudeToMercatorAngle(latitude);
    const double roundtrip =
        WebMercatorProjection::mercatorAngleToGeodeticLatitude(mercator);

    EXPECT_NEAR(latitude, roundtrip, 1e-14);
}

TEST(WebMercatorProjectionTest, GeodeticLatitudeClampsToValidMercatorRange) {
    EXPECT_NEAR(M_PI,
                WebMercatorProjection::geodeticLatitudeToMercatorAngle(
                    M_PI / 2.0),
                1e-14);
    EXPECT_NEAR(-M_PI,
                WebMercatorProjection::geodeticLatitudeToMercatorAngle(
                    -M_PI / 2.0),
                1e-14);
}

TEST(WebMercatorProjectionTest, ProjectAndUnprojectPreserveHeight) {
    const WebMercatorProjection projection(Ellipsoid::WGS84());
    const Cartographic input =
        Cartographic::fromDegrees(116.397, 39.908, 1234.5);

    const Vec3 projected = projection.project(input);
    const Cartographic roundtrip = projection.unproject(projected);

    EXPECT_NEAR(input.longitude(), roundtrip.longitude(), 1e-14);
    EXPECT_NEAR(input.latitude(), roundtrip.latitude(), 1e-14);
    EXPECT_DOUBLE_EQ(input.height(), roundtrip.height());
}

TEST(WebMercatorProjectionTest, ProjectUsesEllipsoidMaximumRadiusLikeCesiumNative) {
    // Source-derived from cesium-native WebMercatorProjection constructor:
    // projected x/y scale is ellipsoid.getMaximumRadius().
    const Ellipsoid ellipsoid(2.0, 5.0, 3.0);
    const WebMercatorProjection projection(ellipsoid);

    const Vec3 projected =
        projection.project(Cartographic::fromRadians(1.0, 0.0, 7.0));

    EXPECT_DOUBLE_EQ(5.0, projected.x());
    EXPECT_DOUBLE_EQ(0.0, projected.y());
    EXPECT_DOUBLE_EQ(7.0, projected.z());
}

TEST(WebMercatorProjectionTest, Unproject2DSetsHeightToZeroLikeCesiumNative) {
    const WebMercatorProjection projection(Ellipsoid::WGS84());
    const Cartographic input =
        Cartographic::fromDegrees(116.397, 39.908, 1234.5);
    const Vec3 projected3D = projection.project(input);

    const Cartographic cartographic =
        projection.unproject(glm::dvec2(projected3D.x(), projected3D.y()));

    EXPECT_NEAR(input.longitude(), cartographic.longitude(), 1e-14);
    EXPECT_NEAR(input.latitude(), cartographic.latitude(), 1e-14);
    EXPECT_DOUBLE_EQ(0.0, cartographic.height());
}

TEST(WebMercatorProjectionTest, ProjectClampsLatitudeToMaximumLikeCesiumNative) {
    const WebMercatorProjection projection(Ellipsoid::WGS84());

    const Vec3 northProjected =
        projection.project(Cartographic::fromRadians(0.0, M_PI / 2.0, 7.0));
    const Vec3 southProjected =
        projection.project(Cartographic::fromRadians(0.0, -M_PI / 2.0, 8.0));

    EXPECT_NEAR(M_PI * Ellipsoid::WGS84().maximumRadius(),
                northProjected.y(),
                1e-7);
    EXPECT_DOUBLE_EQ(7.0, northProjected.z());
    EXPECT_NEAR(-M_PI * Ellipsoid::WGS84().maximumRadius(),
                southProjected.y(),
                1e-7);
    EXPECT_DOUBLE_EQ(8.0, southProjected.z());
}

TEST(WebMercatorProjectionTest, ProjectAndUnprojectRectangleMatchesCesiumNative) {
    const WebMercatorProjection projection(Ellipsoid::WGS84());
    const Rectangle globeRectangle =
        Rectangle::fromDegrees(-120.0, -40.0, -60.0, 30.0);

    const Rectangle projected = projection.project(globeRectangle);
    const Vec3 southwest =
        projection.project(Cartographic(globeRectangle.west(), globeRectangle.south(), 0.0));
    const Vec3 northeast =
        projection.project(Cartographic(globeRectangle.east(), globeRectangle.north(), 0.0));
    EXPECT_NEAR(southwest.x(), projected.west(), 1e-9);
    EXPECT_NEAR(southwest.y(), projected.south(), 1e-9);
    EXPECT_NEAR(northeast.x(), projected.east(), 1e-9);
    EXPECT_NEAR(northeast.y(), projected.north(), 1e-9);

    const Rectangle unprojected = projection.unproject(projected);
    EXPECT_NEAR(globeRectangle.west(), unprojected.west(), 1e-15);
    EXPECT_NEAR(globeRectangle.south(), unprojected.south(), 1e-14);
    EXPECT_NEAR(globeRectangle.east(), unprojected.east(), 1e-15);
    EXPECT_NEAR(globeRectangle.north(), unprojected.north(), 1e-14);
}

TEST(WebMercatorProjectionTest, MaximumProjectedRectangleMatchesCesiumNative) {
    const Rectangle maximum =
        WebMercatorProjection::computeMaximumProjectedRectangle(Ellipsoid::WGS84());

    EXPECT_NEAR(-M_PI * Ellipsoid::WGS84().maximumRadius(),
                maximum.west(),
                1e-9);
    EXPECT_NEAR(-M_PI * Ellipsoid::WGS84().maximumRadius(),
                maximum.south(),
                1e-9);
    EXPECT_NEAR(M_PI * Ellipsoid::WGS84().maximumRadius(),
                maximum.east(),
                1e-9);
    EXPECT_NEAR(M_PI * Ellipsoid::WGS84().maximumRadius(),
                maximum.north(),
                1e-9);
}

TEST(WebMercatorProjectionTest, MaximumGlobeRectangleMatchesCesiumNative) {
    const Rectangle maximum = WebMercatorProjection::maximumGlobeRectangle();

    EXPECT_DOUBLE_EQ(-M_PI, maximum.west());
    EXPECT_DOUBLE_EQ(-WebMercatorProjection::maximumLatitude(),
                     maximum.south());
    EXPECT_DOUBLE_EQ(M_PI, maximum.east());
    EXPECT_DOUBLE_EQ(WebMercatorProjection::maximumLatitude(),
                     maximum.north());
}

TEST(WebMercatorProjectionTest, EqualityUsesCesiumNativeEllipsoidSemantics) {
    const WebMercatorProjection lhs(Ellipsoid::WGS84());
    const WebMercatorProjection same(
        Ellipsoid(6378137.0, 6378137.0, 6356752.3142451793));
    const WebMercatorProjection different(Ellipsoid::UNIT_SPHERE());

    EXPECT_TRUE(lhs == same);
    EXPECT_FALSE(lhs != same);
    EXPECT_FALSE(lhs == different);
    EXPECT_TRUE(lhs != different);
}

TEST(ProjectionTest, GetProjectionEllipsoidMatchesCesiumNativeVariantVisitor) {
    const Projection geographic = GeographicProjection(Ellipsoid::WGS84());
    const Projection webMercator =
        WebMercatorProjection(Ellipsoid::UNIT_SPHERE());

    EXPECT_EQ(Ellipsoid::WGS84(), getProjectionEllipsoid(geographic));
    EXPECT_EQ(Ellipsoid::UNIT_SPHERE(), getProjectionEllipsoid(webMercator));
}

TEST(ProjectionTest, ProjectAndUnprojectPositionUseCesiumNativeVariantVisitor) {
    const Projection projection = WebMercatorProjection(Ellipsoid::WGS84());
    const Cartographic position =
        Cartographic::fromDegrees(12.5, -34.25, 678.0);

    const Vec3 projected = projectPosition(projection, position);
    const Cartographic roundtrip = unprojectPosition(projection, projected);

    EXPECT_NEAR(position.longitude(), roundtrip.longitude(), 1e-14);
    EXPECT_NEAR(position.latitude(), roundtrip.latitude(), 1e-14);
    EXPECT_DOUBLE_EQ(position.height(), roundtrip.height());
}

TEST(ProjectionTest, ProjectAndUnprojectRegionSimplePreservesProjectedRectangleAndHeights) {
    const Projection projection = WebMercatorProjection(Ellipsoid::WGS84());
    const BoundingRegionBuilder::BoundingRegion region{
        Rectangle::fromDegrees(-120.0, -40.0, -60.0, 30.0),
        -25.0,
        1750.0
    };

    const AxisAlignedBox box = projectRegionSimple(projection, region);
    const Rectangle projectedRectangle =
        projectRectangleSimple(projection, region.rectangle);

    EXPECT_NEAR(projectedRectangle.west(), box.minimumX(), 1e-9);
    EXPECT_NEAR(projectedRectangle.south(), box.minimumY(), 1e-9);
    EXPECT_DOUBLE_EQ(region.minimumHeight, box.minimumZ());
    EXPECT_NEAR(projectedRectangle.east(), box.maximumX(), 1e-9);
    EXPECT_NEAR(projectedRectangle.north(), box.maximumY(), 1e-9);
    EXPECT_DOUBLE_EQ(region.maximumHeight, box.maximumZ());

    const BoundingRegionBuilder::BoundingRegion roundtrip =
        unprojectRegionSimple(projection, box);
    EXPECT_NEAR(region.rectangle.west(), roundtrip.rectangle.west(), 1e-15);
    EXPECT_NEAR(region.rectangle.south(), roundtrip.rectangle.south(), 1e-14);
    EXPECT_NEAR(region.rectangle.east(), roundtrip.rectangle.east(), 1e-15);
    EXPECT_NEAR(region.rectangle.north(), roundtrip.rectangle.north(), 1e-14);
    EXPECT_DOUBLE_EQ(region.minimumHeight, roundtrip.minimumHeight);
    EXPECT_DOUBLE_EQ(region.maximumHeight, roundtrip.maximumHeight);
}

TEST(ProjectionTest, ComputeProjectedRectangleSizeMatchesCesiumNativeGlobeCases) {
    const GeographicProjection projection(Ellipsoid::WGS84());
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    double maxHeight = 0.0;

    Rectangle entireGlobe = projectRectangleSimple(
        projection,
        Rectangle::fromDegrees(-180.0, -90.0, 180.0, 90.0));
    glm::dvec2 entireSize = computeProjectedRectangleSize(
        projection,
        entireGlobe,
        maxHeight,
        ellipsoid);
    EXPECT_GT(entireSize.x, ellipsoid.maximumRadius() * 2.0);
    EXPECT_NEAR(ellipsoid.minimumRadius() * 2.0, entireSize.y, 1.0);

    Rectangle westernHemisphere = projectRectangleSimple(
        projection,
        Rectangle::fromDegrees(-180.0, -90.0, 0.0, 90.0));
    glm::dvec2 westernSize = computeProjectedRectangleSize(
        projection,
        westernHemisphere,
        maxHeight,
        ellipsoid);
    EXPECT_NEAR(ellipsoid.maximumRadius() * 2.0, westernSize.x, 1.0);
    EXPECT_NEAR(ellipsoid.minimumRadius() * 2.0, westernSize.y, 1.0);

    Rectangle easternHemisphere = projectRectangleSimple(
        projection,
        Rectangle::fromDegrees(0.0, -90.0, 180.0, 90.0));
    glm::dvec2 easternSize = computeProjectedRectangleSize(
        projection,
        easternHemisphere,
        maxHeight,
        ellipsoid);
    EXPECT_NEAR(ellipsoid.maximumRadius() * 2.0, easternSize.x, 1.0);
    EXPECT_NEAR(ellipsoid.minimumRadius() * 2.0, easternSize.y, 1.0);
}

TEST(ProjectionTest, ComputeProjectedRectangleSizeMatchesCesiumNativeEquatorCases) {
    const GeographicProjection projection(Ellipsoid::WGS84());
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    double maxHeight = 0.0;

    Rectangle crossingEquator = projectRectangleSimple(
        projection,
        Rectangle::fromDegrees(-100.0, -70.0, -80.0, 40.0));
    glm::dvec2 crossingSize = computeProjectedRectangleSize(
        projection,
        crossingEquator,
        maxHeight,
        ellipsoid);
    const double equatorWidth = ellipsoid
        .cartographicToCartesian(Cartographic::fromDegrees(-100.0, 0.0, 0.0))
        .distanceTo(ellipsoid.cartographicToCartesian(
            Cartographic::fromDegrees(-80.0, 0.0, 0.0)));
    EXPECT_NEAR(equatorWidth, crossingSize.x, 1.0);

    Rectangle narrowBand = projectRectangleSimple(
        projection,
        Rectangle::fromDegrees(-180.0, 20.0, 180.0, 40.0));
    glm::dvec2 narrowSize = computeProjectedRectangleSize(
        projection,
        narrowBand,
        maxHeight,
        ellipsoid);
    const double meridianHeight = ellipsoid
        .cartographicToCartesian(Cartographic::fromDegrees(0.0, 20.0, 0.0))
        .distanceTo(ellipsoid.cartographicToCartesian(
            Cartographic::fromDegrees(0.0, 40.0, 0.0)));
    EXPECT_GT(narrowSize.x, ellipsoid.maximumRadius() * 2.0);
    EXPECT_NEAR(meridianHeight, narrowSize.y, 1.0);
}

TEST(ProjectionTest, ComputeProjectedRectangleSizeUsesMaximumHeightLikeCesiumNative) {
    const GeographicProjection projection(Ellipsoid::WGS84());
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const double maxHeight = 1000.0;

    const Rectangle projected = projectRectangleSimple(
        projection,
        Rectangle::fromDegrees(-10.0, 0.0, 10.0, 0.0));

    const glm::dvec2 size = computeProjectedRectangleSize(
        projection,
        projected,
        maxHeight,
        ellipsoid);

    const double expectedWidth = ellipsoid
        .cartographicToCartesian(Cartographic::fromDegrees(-10.0, 0.0, maxHeight))
        .distanceTo(ellipsoid.cartographicToCartesian(
            Cartographic::fromDegrees(10.0, 0.0, maxHeight)));
    const double surfaceWidth = ellipsoid
        .cartographicToCartesian(Cartographic::fromDegrees(-10.0, 0.0, 0.0))
        .distanceTo(ellipsoid.cartographicToCartesian(
            Cartographic::fromDegrees(10.0, 0.0, 0.0)));

    EXPECT_NEAR(expectedWidth, size.x, 1e-6);
    EXPECT_GT(size.x, surfaceWidth);
}

TEST(ProjectionTest, ComputeProjectedRectangleSizeChecksPrimeMeridianWhenProjectedXCrossesZero) {
    // Source-derived from cesium-native CesiumGeospatial/src/Projection.cpp:
    // when projected X crosses zero, the Y size is also measured at X=0.
    // A tri-axial ellipsoid makes this branch observable.
    const Ellipsoid ellipsoid(3.0, 2.0, 4.0);
    const GeographicProjection projection(ellipsoid);
    const double maxHeight = 0.0;

    const Rectangle projected = projectRectangleSimple(
        projection,
        Rectangle::fromDegrees(-60.0, 20.0, 60.0, 40.0));

    const glm::dvec2 size = computeProjectedRectangleSize(
        projection,
        projected,
        maxHeight,
        ellipsoid);

    const double centerMeridianHeight = ellipsoid
        .cartographicToCartesian(Cartographic::fromDegrees(0.0, 20.0, maxHeight))
        .distanceTo(ellipsoid.cartographicToCartesian(
            Cartographic::fromDegrees(0.0, 40.0, maxHeight)));
    const double edgeMeridianHeight = ellipsoid
        .cartographicToCartesian(Cartographic::fromDegrees(-60.0, 20.0, maxHeight))
        .distanceTo(ellipsoid.cartographicToCartesian(
            Cartographic::fromDegrees(-60.0, 40.0, maxHeight)));

    EXPECT_GT(centerMeridianHeight, edgeMeridianHeight);
    EXPECT_NEAR(centerMeridianHeight, size.y, 1e-14);
}
