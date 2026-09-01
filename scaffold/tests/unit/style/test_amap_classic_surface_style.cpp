#include <gtest/gtest.h>

#include "earth_engine/style/AmapClassicStyleInternal.h"
#include "../../helpers/AmapOfficialStyleTestAdapter.h"

using namespace earth_engine;

namespace {

std::array<float, 4> evaluate(int classCode, int subKey,
                              double displayZoom) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Regions);
    EXPECT_TRUE(style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::FillIdentity));
    EXPECT_TRUE(style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::DrawOrder));
    EXPECT_TRUE(style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::ZoomWindow));
    StyleExpression::PropertyMap properties{
        {"amap_class", std::to_string(classCode)},
        {"amap_subkey", std::to_string(subKey)}};
    const auto group = style.fillStyleGroupExpr->evaluate(&properties,
                                                          displayZoom);
    EXPECT_TRUE(group.has_value());
    EXPECT_EQ(StyleValue::Kind::Number, group->kind());
    if (group->number() == 0.0) return {0, 0, 0, 0};
    const auto it = style.fillColorExprByStyleGroup.find(
        static_cast<int>(group->number()));
    EXPECT_NE(style.fillColorExprByStyleGroup.end(), it);
    if (it == style.fillColorExprByStyleGroup.end()) return {};
    const auto value = it->second->evaluate(nullptr, displayZoom);
    EXPECT_TRUE(value.has_value());
    EXPECT_EQ(StyleValue::Kind::Color, value->kind());
    return value->color();
}

}  // namespace

TEST(AmapClassicSurfaceStyleTest, UsesOfficial30001DisplayZoomWindows) {
    // Production identities: sub2/kind63 is water; sub3/kind61 is
    // vegetation.  The expression is keyed by the official subKey and source
    // zoom; kind remains geometry metadata.
    EXPECT_EQ((std::array<float, 4>{0xb4 / 255.0f, 0xeb / 255.0f,
                                    0xaf / 255.0f, 1.0f}),
              evaluate(30001, 3, 9.0));
    EXPECT_EQ((std::array<float, 4>{0xb2 / 255.0f, 0xce / 255.0f,
                                    0xfe / 255.0f, 1.0f}),
              evaluate(30001, 2, 9.0));
    EXPECT_EQ((std::array<float, 4>{0x80 / 255.0f, 0xdf / 255.0f,
                                    0xff / 255.0f, 1.0f}),
              evaluate(30001, 27, 13.0));
    EXPECT_EQ((std::array<float, 4>{0x82 / 255.0f, 0xdc / 255.0f,
                                    0xfa / 255.0f, 1.0f}),
              evaluate(30001, 27, 15.0));
}

TEST(AmapClassicSurfaceStyleTest, PreservesOfficialAlphaAndZoomGates) {
    EXPECT_EQ((std::array<float, 4>{0xee / 255.0f, 0xee / 255.0f,
                                    0xee / 255.0f, 0x7f / 255.0f}),
              evaluate(30002, 22, 13.0));
    EXPECT_FLOAT_EQ(0.0f, evaluate(30002, 1, 14.0)[3]);
    EXPECT_FLOAT_EQ(1.0f, evaluate(30002, 1, 15.0)[3]);
}

TEST(AmapClassicSurfaceStyleTest, SportsGroundsFollowDisplayZoomNotSourceTile) {
    // Official 30002 subKeys 19/20/21 switch from green at AMap z15 to
    // turquoise at z16. Both colors must be reachable from the same z14 tile.
    const auto green = evaluate(30002, 19, 14.0);
    const auto turquoise = evaluate(30002, 19, 15.0);
    EXPECT_EQ((std::array<float, 4>{0xb4 / 255.0f, 0xeb / 255.0f,
                                    0xaf / 255.0f, 1.0f}), green);
    EXPECT_EQ((std::array<float, 4>{0x79 / 255.0f, 0xd5 / 255.0f,
                                    0xc0 / 255.0f, 1.0f}), turquoise);
}

TEST(AmapClassicSurfaceStyleTest, SportsGroundsSwitchAtOfficialFractionalZoom) {
    // Current JSAPI selects the lower integer style record while fraction
    // < .8, then selects the upper record. Source geometry remains z14.
    EXPECT_EQ((std::array<float, 4>{0xb4 / 255.0f, 0xeb / 255.0f,
                                    0xaf / 255.0f, 1.0f}),
              evaluate(30002, 19, 14.79));
    EXPECT_EQ((std::array<float, 4>{0x79 / 255.0f, 0xd5 / 255.0f,
                                    0xc0 / 255.0f, 1.0f}),
              evaluate(30002, 19, 14.80));
    // Football, basketball and the sibling sports-ground identity share the
    // same official records and must not drift independently.
    EXPECT_EQ(evaluate(30002, 19, 14.79), evaluate(30002, 20, 14.79));
    EXPECT_EQ(evaluate(30002, 19, 14.80), evaluate(30002, 21, 14.80));
}

TEST(AmapClassicSurfaceStyleTest, UnknownDataIsTransparentAndUsesCameraZoom) {
    EXPECT_FLOAT_EQ(0.0f, evaluate(30001, 999, 14.0)[3]);
}

TEST(AmapClassicSurfaceStyleTest,
     ReversedOfficialWindowsAreFailClosedRatherThanExecutableStops) {
    // Fixed official PBF records 7454/7455 have windows 14..13 and therefore
    // select no zoom. They must not create identities merely because a Step
    // implementation happens to hide their reversed stops today.
    EXPECT_FLOAT_EQ(0.0f, evaluate(30001, 30, 13.0)[3]);
    EXPECT_FLOAT_EQ(0.0f, evaluate(30001, 31, 13.0)[3]);

    // subKey 34 also has an unreachable 13..10 record plus a valid 14..30
    // record. Preserve the valid official identity and gate only the invalid
    // interval.
    EXPECT_FLOAT_EQ(0.0f, evaluate(30001, 34, 12.0)[3]);
    EXPECT_EQ((std::array<float, 4>{0xe0 / 255.0f, 0xf4 / 255.0f,
                                    0xb1 / 255.0f, 1.0f}),
              evaluate(30001, 34, 13.0));
}

TEST(AmapClassicSurfaceStyleTest,
     EverySurfaceExpressionMatchesEveryOfficialRecordAtEveryZoom) {
    const FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Regions);
    EXPECT_TRUE(amapClassicSurfaceExpressionsMatchOfficialRecordsForTest(
        style));
}

TEST(AmapClassicSurfaceStyleTest, UsesOfficial30003SubKeyColorAndAlpha) {
    EXPECT_EQ((std::array<float, 4>{0x55 / 255.0f, 0x91 / 255.0f,
                                    0xce / 255.0f, 0x72 / 255.0f}),
              evaluate(30003, 3, 16.0));
    EXPECT_EQ((std::array<float, 4>{0xdf / 255.0f, 0x59 / 255.0f,
                                    0x14 / 255.0f, 1.0f}),
              evaluate(30003, 127, 16.0));
    EXPECT_FLOAT_EQ(0.0f, evaluate(30003, 124, 16.0)[3]);
}

TEST(AmapClassicSurfaceStyleTest, IncludesEveryOfficialFieldSevenClass) {
    EXPECT_EQ((std::array<float, 4>{0xcd / 255.0f, 0xea / 255.0f,
                                    0xe9 / 255.0f, 1.0f}),
              evaluate(30004, 1, 12.0));
    EXPECT_EQ((std::array<float, 4>{0x81 / 255.0f, 0xd0 / 255.0f,
                                    0x44 / 255.0f, 1.0f}),
              evaluate(30016, 1, 1.0));
}

TEST(AmapClassicSurfaceStyleTest,
     CommandStyleSeparatesIdentityFromDrawOrderAndRejectsUnknownSubKey) {
    FeatureRenderStyle style;
    style.fillStyleGroupExpr = StyleExpression::literal(777.0);
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Regions);
    EXPECT_TRUE(style.usesOfficialProviderContract());

    StyleExpression::PropertyMap sports{{"amap_class", "30002"},
                                        {"amap_subkey", "19"},
                                        {"amap_draworder", "731"}};
    StyleExpression::PropertyMap unknownSports{{"amap_class", "30002"},
                                               {"amap_subkey", "999"}};
    StyleExpression::PropertyMap callerOwned{{"amap_class", "40000"},
                                             {"amap_subkey", "19"}};
    ASSERT_EQ(30002019.0,
              style.fillStyleGroupExpr->evaluate(&sports, 14.8)->number());
    ASSERT_EQ(0.0,
              style.fillStyleGroupExpr->evaluate(&unknownSports, 14.8)->number());
    ASSERT_EQ(0.0,
              style.fillStyleGroupExpr->evaluate(&callerOwned, 14.8)->number());
    EXPECT_EQ(style.fillColorExprByStyleGroup.end(),
              style.fillColorExprByStyleGroup.find(0));
    EXPECT_TRUE(style.fillStyleGroupExpr->referencesProperties());
    EXPECT_FALSE(style.fillStyleGroupExpr->referencesZoom());
    EXPECT_TRUE(style.fillColorExprByStyleGroup.at(30002019)->referencesZoom());
    EXPECT_FALSE(
        style.fillColorExprByStyleGroup.at(30002019)->referencesProperties());
}

TEST(AmapClassicSurfaceStyleTest, UsesOnlyOfficialBuildingIdentityRecords) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Regions);

    const auto group = [&](const char* subKey) {
        std::unordered_map<std::string, std::string> properties{
            {"amap_class", "55001"}, {"amap_subkey", subKey},
            {"amap_draworder", "777"}};
        return static_cast<int>(style.fillStyleGroupExpr
                                    ->evaluate(&properties, 16.0)
                                    ->number());
    };
    EXPECT_EQ(55001001, group("1"));
    EXPECT_EQ(55001002, group("2"));  // inherits subKey 1 building style
    EXPECT_EQ(0, group("999"));
    ASSERT_TRUE(style.extrusionRoofColorByStyleGroup.count(55001001));
    EXPECT_NEAR(249.0f / 255.0f,
                style.extrusionRoofColorByStyleGroup.at(55001001)[0], 1e-6f);
    EXPECT_EQ(16.0, style.fillMinZoomByStyleGroup.at(55001001));
    EXPECT_EQ(30.0, style.fillMaxZoomByStyleGroup.at(55001001));
}

TEST(AmapClassicSurfaceStyleTest,
     EveryBuildingColorAndWindowMatchesEveryOfficialRecord) {
    const FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Regions);
    EXPECT_TRUE(amapClassicBuildingExpressionsMatchOfficialRecordsForTest(
        style));
}
