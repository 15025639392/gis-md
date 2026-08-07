#include <gtest/gtest.h>
#include "earth_engine/tiling/CrsProfile.h"

using namespace earth_engine;

// ============================================================
// Web Mercator (EPSG:3857)
// ============================================================

TEST(CrsProfileTest, WebMercatorId) {
    const auto& crs = CrsProfile::webMercator();
    EXPECT_EQ("EPSG:3857", crs.id());
    EXPECT_EQ("WebMercator", crs.name());
}

TEST(CrsProfileTest, WebMercatorUnit) {
    const auto& crs = CrsProfile::webMercator();
    EXPECT_EQ(CrsProfile::Unit::Meter, crs.unit());
    EXPECT_TRUE(crs.isProjected());
    EXPECT_FALSE(crs.isGeographic());
}

TEST(CrsProfileTest, WebMercatorRange) {
    const auto& crs = CrsProfile::webMercator();
    auto xr = crs.xRange();
    auto yr = crs.yRange();

    EXPECT_NEAR(-20037508.34, xr[0], 0.1);
    EXPECT_NEAR(20037508.34, xr[1], 0.1);
    EXPECT_NEAR(-20037508.34, yr[0], 0.1);
    EXPECT_NEAR(20037508.34, yr[1], 0.1);
}

// ============================================================
// WGS84 Geographic (EPSG:4326)
// ============================================================

TEST(CrsProfileTest, WGS84GeographicId) {
    const auto& crs = CrsProfile::wgs84Geographic();
    EXPECT_EQ("EPSG:4326", crs.id());
    EXPECT_EQ("WGS84Geographic", crs.name());
}

TEST(CrsProfileTest, WGS84GeographicUnit) {
    const auto& crs = CrsProfile::wgs84Geographic();
    EXPECT_EQ(CrsProfile::Unit::Degree, crs.unit());
    EXPECT_TRUE(crs.isGeographic());
    EXPECT_FALSE(crs.isProjected());
}

TEST(CrsProfileTest, WGS84GeographicRange) {
    const auto& crs = CrsProfile::wgs84Geographic();
    auto xr = crs.xRange();
    auto yr = crs.yRange();

    EXPECT_EQ(-180.0, xr[0]);
    EXPECT_EQ(180.0, xr[1]);
    EXPECT_EQ(-90.0, yr[0]);
    EXPECT_EQ(90.0, yr[1]);
}

// ============================================================
// Singletons
// ============================================================

TEST(CrsProfileTest, SingletonSameInstance) {
    const auto& a = CrsProfile::webMercator();
    const auto& b = CrsProfile::webMercator();
    EXPECT_EQ(&a, &b);
}

TEST(CrsProfileTest, DifferentCrsAreDifferent) {
    const auto& wm = CrsProfile::webMercator();
    const auto& wgs84 = CrsProfile::wgs84Geographic();
    EXPECT_NE(&wm, &wgs84);
    EXPECT_NE(wm.id(), wgs84.id());
}
