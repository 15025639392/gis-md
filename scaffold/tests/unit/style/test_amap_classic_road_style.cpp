#include <gtest/gtest.h>
#include "../../helpers/AmapOfficialStyleTestAdapter.h"

#include "earth_engine/style/AmapClassicRoadStyle.h"

using namespace earth_engine;

namespace {
double numberAt(const StyleExpression::Ptr& expr, double zoom) {
    const auto value = expr->evaluate(nullptr, zoom);
    EXPECT_TRUE(value.has_value());
    EXPECT_EQ(StyleValue::Kind::Number, value->kind());
    return value->number();
}
std::array<float, 4> colorAt(const StyleExpression::Ptr& expr, double zoom) {
    const auto value = expr->evaluate(nullptr, zoom);
    EXPECT_TRUE(value.has_value());
    EXPECT_EQ(StyleValue::Kind::Color, value->kind());
    return value->color();
}
double select(const StyleExpression::Ptr& expr, int classCode, int subKey) {
    StyleExpression::PropertyMap properties{
        {"amap_class", std::to_string(classCode)},
        {"amap_subkey", std::to_string(subKey)},
        {"amap_draworder", "9137"}};
    return expr->evaluate(&properties, 0.0)->number();
}
}  // namespace

TEST(AmapClassicRoadStyleTest, DecodesEveryOfficialJsapiLineType) {
    using Cap = FeatureRenderStyle::LineCap;
    for (int type : {0, 1, 2, 7, 10, 13, 14}) {
        ASSERT_TRUE(amapClassicRoadDashForLineType(type).has_value());
        EXPECT_EQ(0, amapClassicRoadDashForLineType(type)->count);
    }
    EXPECT_EQ(Cap::Square, amapClassicRoadDashForLineType(13)->cap);
    EXPECT_EQ(Cap::Round, amapClassicRoadDashForLineType(14)->cap);
    EXPECT_EQ((std::array<float, 4>{12, 12, 0, 0}),
              amapClassicRoadDashForLineType(6)->lengths);
    EXPECT_EQ((std::array<float, 4>{6, 3, 2, 3}),
              amapClassicRoadDashForLineType(5)->lengths);
    EXPECT_EQ(Cap::Round, amapClassicRoadDashForLineType(15)->cap);
    EXPECT_EQ(Cap::Square, amapClassicRoadDashForLineType(16)->cap);
    for (int unknown : {-1, 17, 18, 99}) {
        const auto style = amapClassicRoadDashForLineType(unknown);
        ASSERT_TRUE(style.has_value());
        EXPECT_EQ(0, style->count)
            << "official getLineTypeStyle default is solid";
        EXPECT_EQ(Cap::Butt, style->cap)
            << "unknown numeric types consume the official default, not a "
               "local fallback";
    }
}

TEST(AmapClassicRoadStyleTest, GeneratedOfficialContractIsTheOnlySelector) {
    const auto line = amapClassicLineStyleGroupExpression();
    const auto label = amapClassicLineLabelStyleGroupExpression();
    for (const auto [classCode, subKey] :
         std::array<std::pair<int, int>, 8>{{
             {20001, 1}, {20020, 1}, {20024, 1}, {20026, 1},
             {20031, 1}, {20032, 1}, {20037, 3}, {20042, 1}}}) {
        EXPECT_DOUBLE_EQ(amapClassicStyleIdentity(classCode, subKey),
                         select(line, classCode, subKey));
    }
    for (const auto [classCode, subKey] :
         std::array<std::pair<int, int>, 3>{{
             {20014, 2}, {20022, 1}, {20040, 1}}}) {
        EXPECT_DOUBLE_EQ(0.0, select(line, classCode, subKey));
        EXPECT_DOUBLE_EQ(amapClassicStyleIdentity(classCode, subKey),
                         select(label, classCode, subKey));
    }
    EXPECT_DOUBLE_EQ(0.0, select(line, 20999, 1));
    EXPECT_DOUBLE_EQ(0.0, select(label, 20999, 1));
    EXPECT_DOUBLE_EQ(0.0, select(line, 20021, 1));
}

TEST(AmapClassicRoadStyleTest, InstallsCompleteGeneratedOfficialTables) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Main);
    EXPECT_TRUE(style.usesOfficialProviderContract());
    EXPECT_TRUE(style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::LineIdentity));
    EXPECT_FALSE(style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::LabelIdentity));
    EXPECT_TRUE(style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::DrawOrder));
    EXPECT_TRUE(style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::ZoomWindow));
    ASSERT_TRUE(style.lineStyleGroupExpr);
    StyleExpression::PropertyMap official{{"amap_class", "20001"},
                                          {"amap_subkey", "1"},
                                          {"amap_draworder", "9137"}};
    EXPECT_DOUBLE_EQ(amapClassicStyleIdentity(20001, 1),
                     style.lineStyleGroupExpr->evaluate(&official, 0)->number());
    EXPECT_EQ(429u, style.lineWidthExprByStyleGroup.size());
    EXPECT_EQ(429u, style.lineCasingWidthExprByStyleGroup.size());
    EXPECT_EQ(429u, style.lineColorExprByStyleGroup.size());
    EXPECT_EQ(429u, style.lineTypeExprByStyleGroup.size());
    EXPECT_EQ(429u, style.lineMaxZoomByStyleGroup.size());
    EXPECT_EQ(385u, style.lineCasingStyleGroups.size());
    EXPECT_EQ(28u, style.lineSolidCapExprByStyleGroup.size());
    EXPECT_EQ(3u, style.lineCasingSolidCapExprByStyleGroup.size());
    EXPECT_TRUE(style.lineSolidCapExprByStyleGroup.count(
        amapClassicStyleIdentity(20001, 1)));
    EXPECT_TRUE(style.lineCasingSolidCapExprByStyleGroup.count(
        amapClassicStyleIdentity(20001, 1)));
    EXPECT_FALSE(style.lineSolidCapExprByStyleGroup.count(
        amapClassicStyleIdentity(20010, 2)));
    EXPECT_TRUE(style.labelSizeExprByStyleGroup.empty());
    EXPECT_TRUE(style.labelMinZoomByStyleGroup.empty());
    EXPECT_TRUE(style.labelMaxZoomByStyleGroup.empty());
    for (int oldAlias : {70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82}) {
        EXPECT_EQ(0u, style.lineWidthExprByStyleGroup.count(oldAlias));
        EXPECT_EQ(0u, style.lineColorExprByStyleGroup.count(oldAlias));
    }
}

TEST(AmapClassicRoadStyleTest, UsesOfficialCurvesAndFractionalSwitch) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Main);
    const auto id = amapClassicStyleIdentity;
    EXPECT_DOUBLE_EQ(8.0, numberAt(style.lineWidthExprByStyleGroup.at(id(20001, 1)), 13.0));
    EXPECT_DOUBLE_EQ(312.0, numberAt(style.lineWidthExprByStyleGroup.at(id(20001, 1)), 21.0));
    EXPECT_DOUBLE_EQ(-6.0, numberAt(style.lineCasingWidthExprByStyleGroup.at(id(20020, 1)), 20.0));
    EXPECT_DOUBLE_EQ(-114.0, numberAt(
        style.lineCasingWidthExprByStyleGroup.at(id(20024, 50)), 1.0));
    EXPECT_DOUBLE_EQ(-5.0, numberAt(
        style.lineCasingWidthExprByStyleGroup.at(id(20025, 1)), 17.0));
    EXPECT_DOUBLE_EQ(3.0, numberAt(style.lineTypeExprByStyleGroup.at(id(20010, 2)), 20.0));
    EXPECT_EQ((std::array<float, 4>{0x6b / 255.0f, 0xb3 / 255.0f,
                                    0x77 / 255.0f, 1.0f}),
              colorAt(style.lineColorExprByStyleGroup.at(id(20020, 1)), 12.0));
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    const auto& label = style.labelSizeExprByStyleGroup.at(id(20001, 1));
    EXPECT_DOUBLE_EQ(11.0, numberAt(label, 10.79));
    EXPECT_DOUBLE_EQ(13.0, numberAt(label, 10.80));
}

TEST(AmapClassicRoadStyleTest,
     CenterStrokeStaysZeroBeforeItsFirstOfficialWidthRecord) {
    const FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Main);
    const auto& curve = style.lineWidthExprByStyleGroup.at(
        amapClassicStyleIdentity(20004, 5));
    // Provider zoom 9 publishes only the secondary/casing pass. The center
    // width first appears at provider zoom 10 and must not back-propagate.
    EXPECT_DOUBLE_EQ(0.0, numberAt(curve, 8.0));
    EXPECT_DOUBLE_EQ(2.0, numberAt(curve, 9.0));

    const auto& longGap = style.lineWidthExprByStyleGroup.at(
        amapClassicStyleIdentity(20023, 1));
    EXPECT_DOUBLE_EQ(0.0, numberAt(longGap, 5.0));
    EXPECT_GT(numberAt(longGap, 8.0), 0.0);
}

TEST(AmapClassicRoadStyleTest,
     CasingColorStaysTransparentBeforeItsFirstOfficialRecord) {
    const FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Main);
    for (const int styleGroup : {
             amapClassicStyleIdentity(20018, 2),
             amapClassicStyleIdentity(20030, 1)}) {
        // Provider zoom 6..8 has a center stroke but no casing. The first
        // casing record is provider zoom 9; its color must not be clamped
        // backwards by Step while centerWidth + borderWidth is still > 0.
        EXPECT_FLOAT_EQ(
            0.0f,
            colorAt(style.lineCasingColorExprByStyleGroup.at(styleGroup),
                    7.0)[3]);
        EXPECT_GT(
            colorAt(style.lineCasingColorExprByStyleGroup.at(styleGroup),
                    8.0)[3],
            0.0f);
    }
}

TEST(AmapClassicRoadStyleTest,
     EveryRoadColorCurveIsTransparentBeforeItsFirstOfficialStop) {
    const FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Main);
    EXPECT_TRUE(amapClassicRoadColorsAreTransparentBeforeFirstStopForTest(
        style));
}

TEST(AmapClassicRoadStyleTest,
     EveryOfficialRoadFieldReachesTheFinalConsumerAtEveryZoom) {
    const FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Main);
    EXPECT_TRUE(amapClassicRoadTablesReachFinalConsumerForTest(style));
}

TEST(AmapClassicRoadStyleTest, RejectsOfficialWidthOnlyIdentity) {
    const auto line = amapClassicLineStyleGroupExpression();
    EXPECT_DOUBLE_EQ(0.0, select(line, 20016, 17));
}

TEST(AmapClassicRoadStyleTest, OfficialVisibilityWindowsDoNotLeak) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Main);
    const int finiteLine = amapClassicStyleIdentity(20016, 1);
    EXPECT_EQ(1.0, style.lineMinZoomByStyleGroup.at(finiteLine));
    EXPECT_EQ(7.0, style.lineMaxZoomByStyleGroup.at(finiteLine));
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Poi);
    const int finiteLabel = amapClassicStyleIdentity(20040, 1);
    EXPECT_EQ(3, style.labelMinZoomByStyleGroup.at(finiteLabel));
    EXPECT_EQ(9, style.labelMaxZoomByStyleGroup.at(finiteLabel));
}

TEST(AmapClassicRoadStyleTest, OfficialTransitColorKeepsItsZoomTransition) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(FeatureRenderLayer::AmapClassicProfile::Main);
    const auto& curve = style.lineColorExprByStyleGroup.at(
        amapClassicStyleIdentity(20015, 194));
    EXPECT_EQ((std::array<float, 4>{0xa4 / 255.0f, 0xb1 / 255.0f,
                                    0xba / 255.0f, 1.0f}), colorAt(curve, 10.0));
    EXPECT_EQ((std::array<float, 4>{0x77 / 255.0f, 0xac / 255.0f,
                                    0xe2 / 255.0f, 0x99 / 255.0f}),
              colorAt(curve, 11.0));
}

TEST(AmapClassicRoadStyleTest, OfficialProfileHasNoCallerStyleInheritance) {
    FeatureRenderStyle style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    EXPECT_TRUE(style.usesOfficialProviderContract());
    EXPECT_TRUE(style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::DrawOrder));
    EXPECT_TRUE(style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::ZoomWindow));
    EXPECT_TRUE(style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::Rank));
    EXPECT_TRUE(style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::LabelIdentity));
    EXPECT_TRUE(style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::LineIdentity));
    ASSERT_TRUE(style.labelStyleGroupExpr);
    ASSERT_FALSE(style.lineLabelLayoutByStyleGroup.empty());
    EXPECT_FLOAT_EQ(0.0f, style.labelSizePx);
    EXPECT_FLOAT_EQ(0.0f, style.labelOffsetPx);
    EXPECT_EQ((std::array<float, 4>{0, 0, 0, 0}), style.labelColor);
    EXPECT_EQ((std::array<float, 4>{0, 0, 0, 0}), style.labelHaloColor);
    EXPECT_EQ(285u, style.lineLabelLayoutByStyleGroup.size());
    EXPECT_TRUE(amapClassicRoadLabelTablesCoverEveryOfficialIdentityForTest(
        style));
    EXPECT_EQ((std::array<float, 4>{0, 0, 0, 0}), style.fillColor);
    EXPECT_EQ((std::array<float, 4>{0, 0, 0, 0}), style.lineColor);
    EXPECT_EQ((std::array<float, 4>{0, 0, 0, 0}), style.pointColor);
    EXPECT_TRUE(style.pointImage.empty());
    EXPECT_FLOAT_EQ(0.0f, style.lineWidthPx);
    EXPECT_FLOAT_EQ(0.0f, style.pointSizePx);
    EXPECT_FLOAT_EQ(0.0f, style.labelSizePx);
    EXPECT_FLOAT_EQ(0.0f, style.labelOffsetPx);
    EXPECT_FLOAT_EQ(0.0f, style.labelHaloPx);
    EXPECT_FALSE(style.fillColorExpr);
    EXPECT_FALSE(style.lineColorExpr);
    EXPECT_FALSE(style.pointColorExpr);
    EXPECT_FALSE(style.pointImageExpr);
    for (const auto& [styleGroup, layout] :
         style.lineLabelLayoutByStyleGroup) {
        (void)styleGroup;
        EXPECT_FLOAT_EQ(0.0f, layout.repeatDistancePx);
        EXPECT_FLOAT_EQ(1.0f / 24.0f, layout.letterSpacingEm);
        EXPECT_FLOAT_EQ(1.0f, layout.paddingXPx);
        EXPECT_FLOAT_EQ(0.0f, layout.paddingYPx);
    }
}
