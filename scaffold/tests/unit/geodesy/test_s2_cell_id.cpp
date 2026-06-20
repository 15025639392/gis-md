#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/S2CellID.h"
#include "earth_engine/core/math/MathUtils.h"
#include "earth_engine/core/math/Rectangle.h"

#include <array>
#include <cmath>

using namespace earth_engine;

namespace {

void expectRectangleNear(const Rectangle& actual,
                         const Rectangle& expected,
                         double epsilon = MathUtils::Epsilon14) {
    EXPECT_NEAR(actual.west(), expected.west(), epsilon);
    EXPECT_NEAR(actual.south(), expected.south(), epsilon);
    EXPECT_NEAR(actual.east(), expected.east(), epsilon);
    EXPECT_NEAR(actual.north(), expected.north(), epsilon);
}

void expectCartographicNear(const Cartographic& actual,
                            double longitude,
                            double latitude,
                            double height = 0.0,
                            double epsilon = MathUtils::Epsilon10) {
    EXPECT_NEAR(longitude, actual.longitude(), epsilon);
    EXPECT_NEAR(latitude, actual.latitude(), epsilon);
    EXPECT_NEAR(height, actual.height(), epsilon);
}

} // namespace

TEST(S2CellIDTest, TokenAndIdSemanticsMatchCesiumNative) {
    EXPECT_TRUE(S2CellID(3458764513820540928ULL).isValid());
    EXPECT_EQ(3458764513820540928ULL, S2CellID::fromToken("3").getID());
    EXPECT_EQ(288230376151711744ULL, S2CellID::fromToken("04").getID());
    EXPECT_EQ(3383782026967071427ULL,
              S2CellID::fromToken("2ef59bd352b93ac3").getID());

    EXPECT_FALSE(S2CellID::fromToken("XX").isValid());
    EXPECT_FALSE(S2CellID::fromToken("LOL").isValid());
    EXPECT_FALSE(S2CellID::fromToken("----").isValid());
    EXPECT_FALSE(S2CellID::fromToken(std::string(17, '9')).isValid());
    EXPECT_FALSE(S2CellID::fromToken("0").isValid());
    EXPECT_FALSE(S2CellID(0ULL).isValid());
    EXPECT_FALSE(S2CellID(~uint64_t{0}).isValid());
    EXPECT_FALSE(S2CellID(
                     0b0010101000000000000000000000000000000000000000000000000000000000ULL)
                     .isValid());

    EXPECT_EQ("3", S2CellID(3458764513820540928ULL).toToken());
    EXPECT_EQ("04", S2CellID(288230376151711744ULL).toToken());
    EXPECT_EQ("2ef59bd352b93ac3",
              S2CellID(3383782026967071427ULL).toToken());
}

TEST(S2CellIDTest, LevelAndFaceMatchCesiumNative) {
    EXPECT_EQ(1, S2CellID(3170534137668829184ULL).getLevel());
    EXPECT_EQ(16, S2CellID(3383782026921377792ULL).getLevel());
    EXPECT_EQ(30, S2CellID(3383782026967071427ULL).getLevel());

    EXPECT_EQ(0, S2CellID::fromToken("1").getFace());
    EXPECT_EQ(1, S2CellID::fromToken("3").getFace());
    EXPECT_EQ(5, S2CellID::fromFaceLevelPosition(5, 0, 0).getFace());
}

TEST(S2CellIDTest, FromQuadtreeTileIdMatchesCesiumNative) {
    const uint8_t face = S2CellID::fromToken("1").getFace();

    EXPECT_EQ(S2CellID::fromToken("1").getID(),
              S2CellID::fromQuadtreeTileID(
                  face,
                  0,
                  0,
                  0)
                  .getID());
    EXPECT_EQ(S2CellID::fromToken("04").getID(),
              S2CellID::fromQuadtreeTileID(
                  face,
                  1,
                  0,
                  0)
                  .getID());
    EXPECT_EQ(S2CellID::fromToken("1c").getID(),
              S2CellID::fromQuadtreeTileID(
                  face,
                  1,
                  1,
                  0)
                  .getID());
    EXPECT_EQ(S2CellID::fromToken("0c").getID(),
              S2CellID::fromQuadtreeTileID(
                  face,
                  1,
                  0,
                  1)
                  .getID());
    EXPECT_EQ(S2CellID::fromToken("14").getID(),
              S2CellID::fromQuadtreeTileID(
                  face,
                  1,
                  1,
                  1)
                  .getID());
}

TEST(S2CellIDTest, OddFacesSwapQuadtreeAxesLikeCesiumNative) {
    const uint8_t face = S2CellID::fromToken("3").getFace();

    EXPECT_EQ(S2CellID::fromFaceLevelPosition(face, 1, 1).getID(),
              S2CellID::fromQuadtreeTileID(
                  face,
                  1,
                  1,
                  0)
                  .getID());
    EXPECT_EQ(S2CellID::fromFaceLevelPosition(face, 1, 3).getID(),
              S2CellID::fromQuadtreeTileID(
                  face,
                  1,
                  0,
                  1)
                  .getID());
}

TEST(S2CellIDTest, ParentAndChildMatchCesiumNativeS2Hierarchy) {
    const S2CellID root = S2CellID::fromToken("1");
    EXPECT_EQ("04", root.getChild(0).toToken());
    EXPECT_EQ("0c", root.getChild(1).toToken());
    EXPECT_EQ("14", root.getChild(2).toToken());
    EXPECT_EQ("1c", root.getChild(3).toToken());

    EXPECT_EQ(root.getID(), root.getChild(0).getParent().getID());
    EXPECT_EQ(root.getID(), root.getChild(1).getParent().getID());
    EXPECT_EQ(root.getID(), root.getChild(2).getParent().getID());
    EXPECT_EQ(root.getID(), root.getChild(3).getParent().getID());

    const S2CellID deep = S2CellID::fromToken("2ef59bd352b93ac3");
    const S2CellID parent = deep.getParent();
    EXPECT_EQ("2ef59bd352b93ac4", parent.toToken());
    EXPECT_EQ(29, parent.getLevel());
    EXPECT_EQ(deep.getID(), parent.getChild(1).getID());
}

TEST(S2CellIDTest, CentersMatchCesiumNative) {
    expectCartographicNear(S2CellID::fromToken("1").getCenter(), 0.0, 0.0);
    expectCartographicNear(
        S2CellID::fromToken("3").getCenter(),
        MathUtils::degreesToRadians(90.0),
        0.0);

    const Cartographic northPole = S2CellID::fromToken("5").getCenter();
    EXPECT_NEAR(MathUtils::degreesToRadians(90.0),
                northPole.latitude(),
                MathUtils::Epsilon10);
    EXPECT_NEAR(0.0, northPole.height(), MathUtils::Epsilon10);

    const Cartographic dateline = S2CellID::fromToken("7").getCenter();
    EXPECT_NEAR(MathUtils::degreesToRadians(180.0),
                std::abs(dateline.longitude()),
                MathUtils::Epsilon10);
    EXPECT_NEAR(0.0, dateline.latitude(), MathUtils::Epsilon10);
    EXPECT_NEAR(0.0, dateline.height(), MathUtils::Epsilon10);

    expectCartographicNear(
        S2CellID::fromToken("9").getCenter(),
        MathUtils::degreesToRadians(-90.0),
        0.0);

    const Cartographic southPole = S2CellID::fromToken("b").getCenter();
    EXPECT_NEAR(MathUtils::degreesToRadians(-90.0),
                southPole.latitude(),
                MathUtils::Epsilon10);
    EXPECT_NEAR(0.0, southPole.height(), MathUtils::Epsilon10);

    expectCartographicNear(
        S2CellID::fromToken("2ef59bd352b93ac3").getCenter(),
        MathUtils::degreesToRadians(105.64131803774308),
        MathUtils::degreesToRadians(-10.490091033598308));
    expectCartographicNear(
        S2CellID::fromToken("1234567").getCenter(),
        MathUtils::degreesToRadians(9.868307318504081),
        MathUtils::degreesToRadians(27.468392925827605));
}

TEST(S2CellIDTest, VerticesMatchCesiumNative) {
    const std::array<Cartographic, 4> vertices =
        S2CellID::fromToken("2ef59bd352b93ac3").getVertices();

    expectCartographicNear(
        vertices[0],
        MathUtils::degreesToRadians(105.64131799299665),
        MathUtils::degreesToRadians(-10.490091077431977));
    expectCartographicNear(
        vertices[1],
        MathUtils::degreesToRadians(105.64131808248949),
        MathUtils::degreesToRadians(-10.490091072946313));
    expectCartographicNear(
        vertices[2],
        MathUtils::degreesToRadians(105.64131808248948),
        MathUtils::degreesToRadians(-10.490090989764633));
    expectCartographicNear(
        vertices[3],
        MathUtils::degreesToRadians(105.64131799299665),
        MathUtils::degreesToRadians(-10.4900909942503));
}

TEST(S2CellIDTest, RootBoundingRectanglesMatchCesiumNative) {
    const double poleMinLatitude =
        std::asin(std::sqrt(1.0 / 3.0)) - 0.5 * MathUtils::Epsilon15;

    expectRectangleNear(
        S2CellID::fromFaceLevelPosition(0, 0, 0).computeBoundingRectangle(),
        Rectangle(
            -MathUtils::PiOverFour,
            -MathUtils::PiOverFour,
            MathUtils::PiOverFour,
            MathUtils::PiOverFour));
    expectRectangleNear(
        S2CellID::fromFaceLevelPosition(1, 0, 0).computeBoundingRectangle(),
        Rectangle(
            MathUtils::PiOverFour,
            -MathUtils::PiOverFour,
            3.0 * MathUtils::PiOverFour,
            MathUtils::PiOverFour));
    expectRectangleNear(
        S2CellID::fromFaceLevelPosition(2, 0, 0).computeBoundingRectangle(),
        Rectangle(
            -MathUtils::OnePi,
            poleMinLatitude,
            MathUtils::OnePi,
            MathUtils::PiOverTwo));
    expectRectangleNear(
        S2CellID::fromFaceLevelPosition(3, 0, 0).computeBoundingRectangle(),
        Rectangle(
            3.0 * MathUtils::PiOverFour,
            -MathUtils::PiOverFour,
            -3.0 * MathUtils::PiOverFour,
            MathUtils::PiOverFour));
    expectRectangleNear(
        S2CellID::fromFaceLevelPosition(4, 0, 0).computeBoundingRectangle(),
        Rectangle(
            -3.0 * MathUtils::PiOverFour,
            -MathUtils::PiOverFour,
            -MathUtils::PiOverFour,
            MathUtils::PiOverFour));
    expectRectangleNear(
        S2CellID::fromFaceLevelPosition(5, 0, 0).computeBoundingRectangle(),
        Rectangle(
            -MathUtils::OnePi,
            -MathUtils::PiOverTwo,
            MathUtils::OnePi,
            -poleMinLatitude));
}

TEST(S2CellIDTest, LevelOneBoundingRectanglesMatchCesiumNative) {
    expectRectangleNear(
        S2CellID::fromFaceLevelPosition(0, 1, 0).computeBoundingRectangle(),
        Rectangle(
            -MathUtils::PiOverFour,
            -MathUtils::PiOverFour,
            0.0,
            0.0));

    const Rectangle polar =
        S2CellID::fromFaceLevelPosition(2, 1, 0).computeBoundingRectangle();
    EXPECT_NEAR(0.0, polar.west(), MathUtils::Epsilon14);
    EXPECT_NEAR(MathUtils::PiOverTwo, polar.east(), MathUtils::Epsilon14);
    EXPECT_LT(polar.south(),
              MathUtils::PiOverFour - MathUtils::OnePi / 20.0);
    EXPECT_NEAR(MathUtils::PiOverTwo, polar.north(), MathUtils::Epsilon14);
}
