#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/S2CellID.h"

using namespace earth_engine;

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
