#include <gtest/gtest.h>

#include "earth_engine/data/AmapSurfaceMaskRasterizer.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

using namespace earth_engine;

namespace {

const Rectangle kTile(0.0, 0.0, 1.0, 1.0);

Cartographic tilePoint(double x, double y) {
    // Raster tile-local y is top-down, Cartographic latitude is north-up.
    return Cartographic(x, 1.0 - y);
}

std::vector<Cartographic> ring(
    std::initializer_list<std::pair<double, double>> points,
    bool explicitlyClose = false) {
    std::vector<Cartographic> out;
    out.reserve(points.size() + (explicitlyClose ? 1 : 0));
    for (const auto& [x, y] : points) out.push_back(tilePoint(x, y));
    if (explicitlyClose && !out.empty()) out.push_back(out.front());
    return out;
}

Feature polygon(std::vector<std::vector<Cartographic>> rings) {
    Feature feature;
    feature.type = GeometryType::Polygon;
    feature.rings = std::move(rings);
    return feature;
}

AmapSurfaceMask smallMask(const Feature& feature, int size = 64) {
    return rasterizeAmapSurfaceMask(
        feature, kTile, AmapSurfaceMaskRasterizerOptions{size, 2});
}

}  // namespace

TEST(AmapSurfaceMaskRasterizer, DefaultsTo256R8WithNorthAtTop) {
    const Feature northWest = polygon({ring({{0.0, 0.0}, {0.5, 0.0},
                                             {0.5, 0.5}, {0.0, 0.5}})});
    const AmapSurfaceMask mask = rasterizeAmapSurfaceMask(northWest, kTile);

    ASSERT_EQ(mask.size, 256);
    ASSERT_EQ(mask.coverage.size(), 256u * 256u);
    EXPECT_EQ(mask.sample(32, 32), 255);    // north-west
    EXPECT_EQ(mask.sample(224, 32), 0);     // north-east
    EXPECT_EQ(mask.sample(32, 224), 0);     // south-west
}

TEST(AmapSurfaceMaskRasterizer, UsesEvenOddForSameWindingHoleAndNestedIsland) {
    // All rings deliberately use the same winding.  The result must depend on
    // nesting parity, not orientation: outer / hole / island = on / off / on.
    const Feature nested = polygon({
        ring({{0.05, 0.05}, {0.95, 0.05}, {0.95, 0.95}, {0.05, 0.95}}),
        ring({{0.20, 0.20}, {0.80, 0.20}, {0.80, 0.80}, {0.20, 0.80}}),
        ring({{0.40, 0.40}, {0.60, 0.40}, {0.60, 0.60}, {0.40, 0.60}}),
    });
    const AmapSurfaceMask mask = smallMask(nested);

    EXPECT_EQ(mask.sample(8, 32), 255) << "outer area";
    EXPECT_EQ(mask.sample(20, 32), 0) << "hole";
    EXPECT_EQ(mask.sample(32, 32), 255) << "nested island";
}

TEST(AmapSurfaceMaskRasterizer, SupportsIndependentFragmentsInOneFeature) {
    const Feature fragments = polygon({
        ring({{0.05, 0.10}, {0.30, 0.10}, {0.30, 0.40}, {0.05, 0.40}}),
        ring({{0.70, 0.60}, {0.95, 0.60}, {0.95, 0.90}, {0.70, 0.90}}),
    });
    const AmapSurfaceMask mask = smallMask(fragments);

    EXPECT_EQ(mask.sample(10, 16), 255);
    EXPECT_EQ(mask.sample(54, 48), 255);
    EXPECT_EQ(mask.sample(32, 32), 0);
}

TEST(AmapSurfaceMaskRasterizer, ImplicitClosureAndExplicitClosureMatch) {
    const auto points =
        std::initializer_list<std::pair<double, double>>{{0.1, 0.1},
                                                         {0.8, 0.2},
                                                         {0.2, 0.9}};
    const AmapSurfaceMask implicit = smallMask(polygon({ring(points, false)}));
    const AmapSurfaceMask explicitMask =
        smallMask(polygon({ring(points, true)}));
    EXPECT_EQ(implicit.coverage, explicitMask.coverage);
    EXPECT_EQ(implicit.sample(20, 24), 255);
}

TEST(AmapSurfaceMaskRasterizer, ScanlineClipsConcaveSplitWithoutFalseBridge) {
    // The polygon is connected only to the west of the tile.  Its intersection
    // with the page is two disjoint horizontal bars.  Clipping a concave ring
    // into one ring would introduce a bridge along x=0 and fill the middle.
    const Feature westConnectedC = polygon({ring({
        {-0.50, 0.15}, {0.30, 0.15}, {0.30, 0.35}, {-0.20, 0.35},
        {-0.20, 0.65}, {0.30, 0.65}, {0.30, 0.85}, {-0.50, 0.85},
    })});
    const AmapSurfaceMask mask = smallMask(westConnectedC);

    EXPECT_EQ(mask.sample(8, 16), 255) << "upper clipped component";
    EXPECT_EQ(mask.sample(8, 48), 255) << "lower clipped component";
    EXPECT_EQ(mask.sample(8, 32), 0) << "no clipping bridge between components";
    EXPECT_EQ(mask.sample(40, 16), 0) << "span is clipped at the east edge";
}

TEST(AmapSurfaceMaskRasterizer, TwoFeaturesAreUnionedInsteadOfParityCancelled) {
    Feature a = polygon({ring({{0.10, 0.10}, {0.70, 0.10},
                               {0.70, 0.70}, {0.10, 0.70}})});
    Feature b = polygon({ring({{0.30, 0.30}, {0.90, 0.30},
                               {0.90, 0.90}, {0.30, 0.90}})});
    const std::vector<const Feature*> features{&a, &b};
    const AmapSurfaceMask mask = rasterizeAmapSurfaceMask(
        features, kTile, AmapSurfaceMaskRasterizerOptions{64, 2});

    EXPECT_EQ(mask.sample(16, 16), 255) << "first-only region";
    EXPECT_EQ(mask.sample(48, 48), 255) << "second-only region";
    EXPECT_EQ(mask.sample(32, 32), 255) << "overlap must stay covered";
}

TEST(AmapSurfaceMaskRasterizer, TwoBySupersamplingProducesEdgeCoverage) {
    const Feature triangle = polygon(
        {ring({{0.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}})});
    const AmapSurfaceMask mask = smallMask(triangle, 32);

    const bool hasPartialCoverage = std::any_of(
        mask.coverage.begin(), mask.coverage.end(),
        [](uint8_t value) { return value > 0 && value < 255; });
    EXPECT_TRUE(hasPartialCoverage);
    EXPECT_EQ(mask.sample(2, 29), 255);
    EXPECT_EQ(mask.sample(29, 2), 0);
}

TEST(AmapSurfaceMaskRasterizer, TileKeyUsesAmapGeographicBoundsAndRejectsRange) {
    const TileKey key{"Amap-Geographic", 2, 2, 1};
    const Rectangle bounds = amapSurfaceMaskTileRectangle(key);
    EXPECT_NEAR(bounds.westDegrees(), 0.0, 1e-12);
    EXPECT_NEAR(bounds.eastDegrees(), 90.0, 1e-12);
    EXPECT_NEAR(bounds.northDegrees(), 45.0, 1e-12);
    EXPECT_NEAR(bounds.southDegrees(), 0.0, 1e-12);

    Feature wholeTile = polygon({{
        bounds.getNorthwest(), bounds.getNortheast(), bounds.getSoutheast(),
        bounds.getSouthwest(),
    }});
    const AmapSurfaceMask valid = rasterizeAmapSurfaceMask(
        std::vector<const Feature*>{&wholeTile}, key,
        AmapSurfaceMaskRasterizerOptions{32, 2});
    EXPECT_EQ(valid.sample(16, 16), 255);

    const Rectangle invalid =
        amapSurfaceMaskTileRectangle(TileKey{"Amap-Geographic", 2, 4, 0});
    EXPECT_TRUE(invalid.isEmpty());
    const AmapSurfaceMask empty = rasterizeAmapSurfaceMask(
        std::vector<const Feature*>{&wholeTile},
        TileKey{"Amap-Geographic", 2, 4, 0});
    EXPECT_TRUE(empty.empty());
}

TEST(AmapSurfaceMaskRasterizer, RectangleEntryPointHandlesAntimeridian) {
    const Rectangle wrapped = Rectangle::fromDegrees(170.0, -10.0,
                                                      -170.0, 10.0);
    Feature acrossDateLine;
    acrossDateLine.type = GeometryType::Polygon;
    acrossDateLine.rings = {{
        Cartographic::fromDegrees(175.0, 5.0),
        Cartographic::fromDegrees(-175.0, 5.0),
        Cartographic::fromDegrees(-175.0, -5.0),
        Cartographic::fromDegrees(175.0, -5.0),
    }};
    Feature aroundGreenwich;
    aroundGreenwich.type = GeometryType::Polygon;
    aroundGreenwich.rings = {{
        Cartographic::fromDegrees(-5.0, 5.0),
        Cartographic::fromDegrees(5.0, 5.0),
        Cartographic::fromDegrees(5.0, -5.0),
        Cartographic::fromDegrees(-5.0, -5.0),
    }};

    const AmapSurfaceMask inside = rasterizeAmapSurfaceMask(
        acrossDateLine, wrapped, AmapSurfaceMaskRasterizerOptions{32, 2});
    EXPECT_EQ(inside.sample(16, 16), 255);

    const AmapSurfaceMask outside = rasterizeAmapSurfaceMask(
        aroundGreenwich, wrapped, AmapSurfaceMaskRasterizerOptions{32, 2});
    EXPECT_EQ(outside.sample(16, 16), 0)
        << "0 degrees must not wrap into a narrow date-line page";
}

TEST(AmapSurfaceMaskRasterizer, IgnoresNullNonPolygonAndDegenerateInput) {
    Feature line;
    line.type = GeometryType::LineString;
    line.rings = {ring({{0.0, 0.5}, {1.0, 0.5}})};
    Feature degenerate = polygon({ring({{0.1, 0.1}, {0.2, 0.2}})});
    const std::vector<const Feature*> features{nullptr, &line, &degenerate};
    const AmapSurfaceMask mask = rasterizeAmapSurfaceMask(
        features, kTile, AmapSurfaceMaskRasterizerOptions{32, 2});
    ASSERT_EQ(mask.coverage.size(), 32u * 32u);
    EXPECT_TRUE(std::all_of(mask.coverage.begin(), mask.coverage.end(),
                            [](uint8_t value) { return value == 0; }));
}
