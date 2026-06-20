#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/QuadtreeGeometricError.h"
#include "earth_engine/core/math/Rectangle.h"

#include <cmath>

using namespace earth_engine;

TEST(QuadtreeGeometricErrorTest, MatchesCesiumNativeMaxErrorFormula) {
    const double expected = Ellipsoid::WGS84().maximumRadius() * 0.25 / 65.0;

    EXPECT_DOUBLE_EQ(expected,
                     calcQuadtreeMaxGeometricError(Ellipsoid::WGS84()));
}

TEST(QuadtreeGeometricErrorTest, ComputesCesiumNativeDerivedTerrainErrors) {
    const Rectangle rectangle(-M_PI, -M_PI * 0.25, 0.0, M_PI * 0.25);
    const double maxError =
        calcQuadtreeMaxGeometricError(Ellipsoid::WGS84());

    EXPECT_DOUBLE_EQ(5.0 * maxError * rectangle.width(),
                     calcQuadtreeSkirtHeight(Ellipsoid::WGS84(), rectangle));
    EXPECT_DOUBLE_EQ(
        8.0 * maxError * rectangle.width(),
        calcLayerJsonTerrainGeometricError(Ellipsoid::WGS84(), rectangle));
}
