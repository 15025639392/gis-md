#include <gtest/gtest.h>

#include "earth_engine/providers/AmapSurfaceMaskImageryProvider.h"
#include "earth_engine/providers/VectorSurfaceFillRasterizer.h"
#include "earth_engine/data/Feature.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/platform/bridge/PlatformBridge.h"

#include <memory>
#include <string>
#include <vector>

using namespace earth_engine;

namespace {

Feature amapPolygon(std::vector<std::vector<Cartographic>> rings,
                    int classCode, int subKey, int drawOrder = 0) {
    Feature f;
    f.type = GeometryType::Polygon;
    f.rings = std::move(rings);
    f.properties["amap_class"] = std::to_string(classCode);
    f.properties["amap_subkey"] = std::to_string(subKey);
    f.properties["amap_draworder"] = std::to_string(drawOrder);
    return f;
}

std::vector<Cartographic> ring(double w, double s, double e, double n) {
    return {Cartographic(w, s), Cartographic(e, s),
            Cartographic(e, n), Cartographic(w, n)};
}

bool samePixels(const DecodedImage& a, const DecodedImage& b) {
    return a.width == b.width && a.height == b.height &&
           a.pixels == b.pixels;
}

} // namespace

// The generic surface-fill rasterizer, fed the sealed AMap resolver, must
// produce byte-identical pages to the existing Amap-only makeAmapSurfaceMaskImage
// for the same features.  This is the safety net for retiring the Amap-specific
// rasterizer path in favor of the reusable VectorSurfaceFillImageryProvider.
TEST(VectorSurfaceFill, GenericRasterizerIsAmapEquivalent) {
    auto features = std::make_shared<const std::vector<Feature>>();
    const Rectangle bounds(0.10, 0.10, 0.12, 0.12);
    const double zoom = 12.0;
    const auto projection =
        AmapSurfaceMaskRasterizerOptions::Projection::Geographic;

    // Amap-equivalent: both paths must stay empty for non-polygons / unknown
    // identity / extrusions.
    Feature point;
    point.type = GeometryType::Point;
    Feature extrude = amapPolygon({ring(0.105, 0.105, 0.115, 0.115)}, 55001, 1);
    extrude.properties["amap_height"] = "20";
    auto sparse = std::make_shared<const std::vector<Feature>>(
        std::vector<Feature>{point, extrude});
    auto sparseAmap = makeAmapSurfaceMaskImage(sparse, bounds, zoom, projection);
    auto sparseGeneric = rasterizeSurfaceFill(
        sparse, bounds, zoom, amapSurfaceFillResolver(), projection);
    ASSERT_TRUE(sparseAmap && sparseGeneric);
    EXPECT_TRUE(samePixels(*sparseAmap, *sparseGeneric));

    // Water (30001/2) + land (30001/1) at different draw orders, overlapping.
    auto water = amapPolygon({ring(0.105, 0.105, 0.115, 0.115)}, 30001, 2, 5);
    auto land = amapPolygon({ring(0.108, 0.108, 0.119, 0.119)}, 30001, 1, 2);
    auto full = std::make_shared<const std::vector<Feature>>(
        std::vector<Feature>{water, land});
    auto fullAmap = makeAmapSurfaceMaskImage(full, bounds, zoom, projection);
    auto fullGeneric = rasterizeSurfaceFill(
        full, bounds, zoom, amapSurfaceFillResolver(), projection);
    ASSERT_TRUE(fullAmap && fullGeneric);
    EXPECT_TRUE(samePixels(*fullAmap, *fullGeneric))
        << "generic rasterizer with the AMap resolver must match Amap output";
}
