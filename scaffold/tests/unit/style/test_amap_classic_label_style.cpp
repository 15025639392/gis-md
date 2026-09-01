#include <gtest/gtest.h>
#include <algorithm>
#include <map>
#include <optional>

#include "earth_engine/style/AmapClassicLabelStyleInternal.h"
#include "earth_engine/style/AmapClassicStyleInternal.h"
#include "../../helpers/AmapOfficialStyleTestAdapter.h"

using namespace earth_engine;

namespace {
int adminIdentity(const FeatureRenderStyle& style, const char* subKey) {
    const auto it = style.labelStyleGroupByProperty.find(
        std::string("10002:") + subKey);
    return it == style.labelStyleGroupByProperty.end() ? 0 : it->second;
}

int poiIdentity(const FeatureRenderStyle& style, const char* subKey) {
    const auto it = style.labelStyleGroupByProperty.find(
        std::string("12024:") + subKey);
    return it == style.labelStyleGroupByProperty.end() ? 0 : it->second;
}
}  // namespace

TEST(AmapClassicLabelStyleTest, SeparatesTownAndDistrictTextWithoutPointGroups) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    EXPECT_EQ(amapClassicLabelIdentity(10002, 9), adminIdentity(style, "9"));
    EXPECT_EQ(amapClassicLabelIdentity(10002, 32), adminIdentity(style, "32"));
    EXPECT_EQ(amapClassicLabelIdentity(10002, 37), adminIdentity(style, "37"));
    EXPECT_EQ(0, adminIdentity(style, "999"));
    EXPECT_EQ((std::array<float, 4>{0x3d / 255.0f, 0x3d / 255.0f,
                                    0x3d / 255.0f, 1.0f}),
              style.labelColorExprByStyleGroup.at(
                  amapClassicLabelIdentity(10002, 9))
                  ->evaluate(nullptr, 12)->color());
}

TEST(AmapClassicLabelStyleTest,
     AdministrativeInstallerIsACompleteFailClosedPointContract) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    EXPECT_TRUE(style.usesOfficialProviderContract());
    EXPECT_TRUE(style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::DrawOrder));
    EXPECT_TRUE(style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::ZoomWindow));
    EXPECT_TRUE(style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::Rank));
    EXPECT_TRUE(style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::PointIdentity));
    EXPECT_TRUE(style.requiresOfficial(
        FeatureRenderStyle::OfficialRequirement::LabelIdentity));
    ASSERT_TRUE(style.pointIdentityValidator);
    ASSERT_TRUE(style.pointStyleResolver);
    EXPECT_TRUE(style.pointIdentityValidator("10002", "37"));
    EXPECT_FALSE(style.pointIdentityValidator("10002", "999"));
}

TEST(AmapClassicLabelStyleTest, DistrictSizeUsesOfficialDisplayZoomSteps) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    const auto expr = style.labelSizeExprByStyleGroup.at(
        amapClassicLabelIdentity(10002, 37));
    EXPECT_DOUBLE_EQ(12.0, expr->evaluate(nullptr, 8.0)->number());
    EXPECT_DOUBLE_EQ(13.0, expr->evaluate(nullptr, 10.0)->number());
    EXPECT_DOUBLE_EQ(14.0, expr->evaluate(nullptr, 11.0)->number());
}

TEST(AmapClassicLabelStyleTest, AdministrativeRecordsSwitchAtFractionalZoomPointEight) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);

    const auto district = style.labelSizeExprByStyleGroup.at(
        amapClassicLabelIdentity(10002, 37));
    EXPECT_DOUBLE_EQ(12.0, district->evaluate(nullptr, 9.79)->number());
    EXPECT_DOUBLE_EQ(13.0, district->evaluate(nullptr, 9.80)->number());

    const auto province = style.labelSizeExprByStyleGroup.at(
        amapClassicLabelIdentity(10002, 32));
    EXPECT_DOUBLE_EQ(10.0, province->evaluate(nullptr, 3.79)->number());
    EXPECT_DOUBLE_EQ(12.0, province->evaluate(nullptr, 3.80)->number());
}

TEST(AmapClassicLabelStyleTest, UsesOfficialAdministrativeHaloFamilies) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    ASSERT_TRUE(style.labelHaloColorExprByStyleGroup.count(
        amapClassicLabelIdentity(10002, 37)));
    ASSERT_TRUE(style.labelHaloColorExprByStyleGroup.count(
        amapClassicLabelIdentity(10002, 9)));
    ASSERT_TRUE(style.labelHaloColorExprByStyleGroup.count(
        amapClassicLabelIdentity(10002, 32)));
    EXPECT_EQ((std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}),
              style.labelHaloColorExprByStyleGroup.at(
                  amapClassicLabelIdentity(10002, 37))
                  ->evaluate(nullptr, 10.0)->color());
    EXPECT_EQ((std::array<float, 4>{1.0f, 1.0f, 1.0f, 0xcc / 255.0f}),
              style.labelHaloColorExprByStyleGroup.at(
                  amapClassicLabelIdentity(10002, 9))
                  ->evaluate(nullptr, 10.0)->color());
    EXPECT_EQ((std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}),
              style.labelHaloColorExprByStyleGroup.at(
                  amapClassicLabelIdentity(10002, 32))
                  ->evaluate(nullptr, 4.0)->color());
}

TEST(AmapClassicLabelStyleTest, ProvinceLabelUsesOfficialZoomSteps) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    const auto expr = style.labelSizeExprByStyleGroup.at(
        amapClassicLabelIdentity(10002, 32));
    EXPECT_DOUBLE_EQ(10.0, expr->evaluate(nullptr, 3.0)->number());
    EXPECT_DOUBLE_EQ(12.0, expr->evaluate(nullptr, 4.0)->number());
    EXPECT_DOUBLE_EQ(16.0, expr->evaluate(nullptr, 6.0)->number());
    EXPECT_DOUBLE_EQ(20.0, expr->evaluate(nullptr, 9.0)->number());
}

TEST(AmapClassicLabelStyleTest, PoiFamiliesUseOfficialTextColors) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    for (int subKey : {2, 19, 561, 684, 1356, 63, 201, 1241, 1250})
        EXPECT_EQ(amapClassicLabelIdentity(12024, subKey),
                  poiIdentity(style, std::to_string(subKey).c_str()));
    EXPECT_EQ((std::array<float, 4>{0x2f / 255.0f, 0x7d / 255.0f,
                                    0xeb / 255.0f, 1.0f}),
              style.labelColorExprByStyleGroup.at(
                  amapClassicLabelIdentity(12024, 1356))
                  ->evaluate(nullptr, 10.0)->color());
    EXPECT_EQ((std::array<float, 4>{1.0f, 1.0f, 1.0f, 0xcc / 255.0f}),
              style.labelHaloColorExprByStyleGroup.at(
                  amapClassicLabelIdentity(12024, 1356))
                  ->evaluate(nullptr, 10.0)->color());
    EXPECT_DOUBLE_EQ(
        9.0, style.labelSizeExprByStyleGroup.at(
                 amapClassicLabelIdentity(12024, 1250))
                 ->evaluate(nullptr, 10.0)->number());
    EXPECT_EQ((std::array<float, 4>{0x3d / 255.0f, 0x3d / 255.0f,
                                    0x3d / 255.0f, 1.0f}),
              style.labelColorExprByStyleGroup.at(
                  amapClassicLabelIdentity(12024, 1241))
                  ->evaluate(nullptr, 10.0)->color());
}

TEST(AmapClassicLabelStyleTest, CoversOfficialPoiTaxonomyWithCompactGroups) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);

    EXPECT_EQ("amap_class", style.labelStyleGroupPropertyA);
    EXPECT_EQ("amap_subkey", style.labelStyleGroupPropertyB);
    EXPECT_EQ(2010u, style.labelStyleGroupByProperty.size());
    EXPECT_EQ(amapClassicLabelIdentity(12024, 1178),
              style.labelStyleGroupByProperty.at("12024:1178"));
    EXPECT_EQ(amapClassicLabelIdentity(12024, 1266),
              style.labelStyleGroupByProperty.at("12024:1266"));
    EXPECT_EQ(amapClassicLabelIdentity(12024, 1255),
              style.labelStyleGroupByProperty.at("12024:1255"));
    EXPECT_EQ(amapClassicLabelIdentity(12024, 1230),
              style.labelStyleGroupByProperty.at("12024:1230"));
    EXPECT_EQ(amapClassicLabelIdentity(12024, 1249),
              style.labelStyleGroupByProperty.at("12024:1249"));
    EXPECT_EQ(0u, style.labelStyleGroupByProperty.count("12024:193"));

    EXPECT_DOUBLE_EQ(9.0, style.labelSizeExprByStyleGroup.at(
                                  amapClassicLabelIdentity(12024, 1230))
                              ->evaluate(nullptr, 10.0)->number());
    EXPECT_DOUBLE_EQ(10.0, style.labelSizeExprByStyleGroup.at(
                                   amapClassicLabelIdentity(12024, 1249))
                               ->evaluate(nullptr, 10.0)->number());
    EXPECT_FLOAT_EQ(0.0f, style.labelHaloColorExprByStyleGroup.at(
                                amapClassicLabelIdentity(12024, 1230))
                                ->evaluate(nullptr, 10.0)->color()[3]);
    EXPECT_FLOAT_EQ(0.0f, style.labelHaloColorExprByStyleGroup.at(
                                amapClassicLabelIdentity(12024, 1249))
                                ->evaluate(nullptr, 10.0)->color()[3]);
}

TEST(AmapClassicLabelStyleTest, UnorderedOfficialRecordsUseFullZoomUnion) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    const int identity = amapClassicLabelIdentity(12025, 4);
    EXPECT_EQ(1.0, style.labelMinZoomByStyleGroup.at(identity));
    EXPECT_EQ(30.0, style.labelMaxZoomByStyleGroup.at(identity));
    EXPECT_DOUBLE_EQ(12.0, style.labelSizeExprByStyleGroup.at(identity)
                                ->evaluate(nullptr, 17.0)->number());
}

TEST(AmapClassicLabelStyleTest, CoversAllOfficialAdministrativeSubKeys) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    const size_t administrativeCount = std::count_if(
        style.labelStyleGroupByProperty.begin(),
        style.labelStyleGroupByProperty.end(), [](const auto& entry) {
            return entry.first.rfind("10002:", 0) == 0;
        });
    EXPECT_EQ(29u, administrativeCount);
    EXPECT_EQ(amapClassicLabelIdentity(10002, 5),
              style.labelStyleGroupByProperty.at("10002:5"));
    EXPECT_EQ(amapClassicLabelIdentity(10002, 32),
              style.labelStyleGroupByProperty.at("10002:32"));
    EXPECT_EQ(amapClassicLabelIdentity(10002, 31),
              style.labelStyleGroupByProperty.at("10002:31"));
    EXPECT_DOUBLE_EQ(12.0, style.labelSizeExprByStyleGroup.at(
                                   amapClassicLabelIdentity(10002, 5))
                               ->evaluate(nullptr, 4.0)->number());
    EXPECT_DOUBLE_EQ(18.0, style.labelSizeExprByStyleGroup.at(
                                   amapClassicLabelIdentity(10002, 5))
                               ->evaluate(nullptr, 9.0)->number());
    EXPECT_DOUBLE_EQ(20.0, style.labelSizeExprByStyleGroup.at(
                                   amapClassicLabelIdentity(10002, 31))
                               ->evaluate(nullptr, 9.0)->number());
    EXPECT_DOUBLE_EQ(10.0, style.labelSizeExprByStyleGroup.at(
                                   amapClassicLabelIdentity(10002, 32))
                               ->evaluate(nullptr, 3.0)->number());
    EXPECT_DOUBLE_EQ(20.0, style.labelSizeExprByStyleGroup.at(
                                   amapClassicLabelIdentity(10002, 32))
                               ->evaluate(nullptr, 9.0)->number());
    EXPECT_FLOAT_EQ(0xcc / 255.0f,
                    style.labelHaloColorExprByStyleGroup.at(
                        amapClassicLabelIdentity(10002, 32))
                        ->evaluate(nullptr, 3.0)->color()[3]);
    EXPECT_FLOAT_EQ(1.0f,
                    style.labelHaloColorExprByStyleGroup.at(
                        amapClassicLabelIdentity(10002, 32))
                        ->evaluate(nullptr, 4.0)->color()[3]);
}

TEST(AmapClassicLabelStyleTest, PoiIconsUseOfficialLateBoundAtlasContract) {
    FeatureRenderStyle style;
    style.pointColor = {0.9f, 0.4f, 0.1f, 1.0f};
    style.pointColorExpr = StyleExpression::literal(style.pointColor);
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    EXPECT_TRUE(style.usesOfficialProviderContract());
    EXPECT_FALSE(style.pointColorExpr);
    EXPECT_FALSE(style.pointImageExpr);
    EXPECT_FALSE(style.pointSizeExpr);
    EXPECT_TRUE(style.pointImage.empty());
    EXPECT_TRUE(style.pointStyleResolver);
    const auto railwayIcon = style.pointStyleResolver(
        "12024", "1356", "车站", 10.0, 2.0f);
    EXPECT_TRUE(railwayIcon.enabled);
    EXPECT_EQ("amap-icons-1-89", railwayIcon.image);
    EXPECT_FLOAT_EQ(21.0f, railwayIcon.sizePx);
    EXPECT_FALSE(style.labelOffsetExpr);

    EXPECT_FALSE(style.pointStyleResolver("12024", "1241", "文本", 10.0, 2.0f)
                     .enabled);
    EXPECT_FALSE(style.pointStyleResolver("99999", "1", "未知", 10.0, 2.0f)
                     .enabled);
    EXPECT_TRUE(style.pointStyleResolver(
        "12024", "1178", "机场", 10.0, 2.0f).enabled);
}

TEST(AmapClassicLabelStyleTest, IconMappingUsesOfficialOneBasedCellsAndZoom) {
    const auto city = resolveAmapClassicPoiIconStyle(10002, 5, 4.0);
    ASSERT_TRUE(city.enabled);
    EXPECT_EQ(1, city.atlas);
    EXPECT_EQ(107, city.iconIndex);
    EXPECT_EQ(64, city.cellWidth);
    EXPECT_EQ(64, city.cellHeight);
    EXPECT_EQ(512, city.atlasWidth);
    EXPECT_EQ(1024, city.atlasHeight);
    EXPECT_FLOAT_EQ(21.0f, city.displayWidthPx);
    EXPECT_FLOAT_EQ(10.0f, city.anchorXPx);
    EXPECT_FLOAT_EQ(10.0f, city.anchorYPx);
    EXPECT_EQ("amap-icons-1-107", city.frameName());

    EXPECT_FALSE(resolveAmapClassicPoiIconStyle(10002, 32, 3.0).enabled);
    EXPECT_FALSE(resolveAmapClassicPoiIconStyle(10002, 32, 3.79).enabled);
    EXPECT_EQ(106,
              resolveAmapClassicPoiIconStyle(10002, 32, 3.80).iconIndex);
    EXPECT_EQ(106, resolveAmapClassicPoiIconStyle(10002, 32, 4.0).iconIndex);
    EXPECT_EQ(105, resolveAmapClassicPoiIconStyle(10002, 32, 5.0).iconIndex);
    EXPECT_EQ(106, resolveAmapClassicPoiIconStyle(10002, 32, 6.0).iconIndex);
    // q9t/q8t is a text-measured, dynamically stretched background in the
    // official formatter. Until that full contract is available it must not
    // degrade into the removed fixed 32px fallback icon.
    EXPECT_FALSE(resolveAmapClassicPoiIconStyle(12024, 1230, 20.0).enabled);
    EXPECT_FALSE(resolveAmapClassicPoiIconStyle(12024, 1249, 10.0).enabled);
    EXPECT_FALSE(resolveAmapClassicPoiIconStyle(10002, 11, 20.0).enabled);
    // W9t/labelType 200 records omit atlas/cell geometry in the fixed PBF.
    // The current official manifest has no icons_200 bitmap resource; do not
    // invent the runtime's generic 64px defaults or synthesize a local icon.
    EXPECT_FALSE(resolveAmapClassicPoiIconStyle(12025, 4, 10.0).enabled);
    EXPECT_FALSE(resolveAmapClassicPoiIconStyle(12025, 8, 20.0).enabled);
    EXPECT_FALSE(resolveAmapClassicPoiIconStyle(10031, 88, 20.0).enabled);

    const auto dynamic =
        resolveAmapClassicPoiDynamicBackgroundStyle(12024, 1230, 20.0);
    ASSERT_TRUE(dynamic.enabled);
    EXPECT_EQ(4, dynamic.atlas);
    EXPECT_EQ(73, dynamic.iconIndex);
    EXPECT_EQ("amap-icons-4-73", dynamic.frameName());
    EXPECT_FLOAT_EQ(0.0f, dynamic.displayWidthPx);
    EXPECT_FLOAT_EQ(0.0f, dynamic.displayHeightPx);
    EXPECT_FALSE(
        resolveAmapClassicPoiDynamicBackgroundStyle(12024, 1230, 15.0)
            .enabled);

    const auto atlas50 = amapClassicPoiIconFrames(50);
    const auto frame43 = std::find_if(
        atlas50.begin(), atlas50.end(),
        [](const auto& frame) { return frame.iconIndex == 43; });
    ASSERT_NE(atlas50.end(), frame43);
    EXPECT_EQ(48, frame43->cellWidth);
    EXPECT_EQ(48, frame43->cellHeight);
    EXPECT_EQ(512, frame43->atlasWidth);
    EXPECT_EQ(1024, frame43->atlasHeight);

    EXPECT_EQ((std::vector<int>{1, 4, 7, 8, 9, 11, 23, 24, 26, 50, 51,
                                53, 64, 69, 71, 103, 106}),
              amapClassicPoiIconAtlases());
    EXPECT_FALSE(amapClassicPoiIconFrames(9).empty());
    EXPECT_FALSE(amapClassicPoiIconFrames(64).empty());
}

TEST(AmapClassicLabelStyleTest, OfficialGuideShieldFramesComeFromField8) {
    const auto national = resolveAmapClassicPoiIconStyle(
        40001, 110100, 10.0);
    ASSERT_TRUE(national.enabled);
    EXPECT_EQ(1, national.atlas);
    EXPECT_EQ(114, national.iconIndex);
    EXPECT_EQ(64, national.cellWidth);
    EXPECT_EQ(64, national.cellHeight);
    EXPECT_FLOAT_EQ(24.0f, national.displayWidthPx);
    EXPECT_FLOAT_EQ(24.0f, national.displayHeightPx);
    EXPECT_TRUE(isAmapClassicPoiIdentity(40001, 110105));
    EXPECT_FALSE(isAmapClassicPoiIdentity(40001, 110106));
}

TEST(AmapClassicLabelStyleTest,
     OfficialGuideShieldWidthSelectsBinaryRuntimeScaleBranch) {
    const FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Poi);
    ASSERT_TRUE(style.pointStyleResolver);
    const auto retinaFour = style.pointStyleResolver(
        "40001", "110100", "一二三四", 10.0, 2.0f);
    const auto retinaFive = style.pointStyleResolver(
        "40001", "110100", "一二三四五", 10.0, 2.0f);
    const auto oneXFour = style.pointStyleResolver(
        "40001", "110100", "一二三四", 10.0, 1.0f);
    const auto oneXFive = style.pointStyleResolver(
        "40001", "110100", "一二三四五", 10.0, 1.0f);
    ASSERT_TRUE(retinaFour.labelLayout);
    ASSERT_TRUE(retinaFive.labelLayout);
    ASSERT_TRUE(oneXFour.labelLayout);
    ASSERT_TRUE(oneXFive.labelLayout);
    EXPECT_FLOAT_EQ(24.0f, retinaFour.labelLayout->iconWidthPx);
    EXPECT_FLOAT_EQ(30.0f, retinaFive.labelLayout->iconWidthPx);
    EXPECT_FLOAT_EQ(24.0f * 9.0f / 7.0f,
                    oneXFour.labelLayout->iconWidthPx);
    EXPECT_FLOAT_EQ(30.0f * 9.0f / 7.0f,
                    oneXFive.labelLayout->iconWidthPx);
    EXPECT_FLOAT_EQ(retinaFour.labelLayout->iconWidthPx * 0.5f,
                    retinaFour.labelLayout->iconAnchorXPx);
    EXPECT_FLOAT_EQ(oneXFive.labelLayout->iconWidthPx * 0.5f,
                    oneXFive.labelLayout->iconAnchorXPx);

    // JavaScript x.length counts UTF-16 code units. Five astral scalars are
    // therefore length 10, not five Unicode codepoints: T=10/4=2.5.
    const std::string fiveAstral =
        "\xF0\xA0\x80\x80\xF0\xA0\x80\x81\xF0\xA0\x80\x82"
        "\xF0\xA0\x80\x83\xF0\xA0\x80\x84";
    const auto retinaAstral = style.pointStyleResolver(
        "40001", "110100", fiveAstral, 10.0, 2.0f);
    const auto oneXAstral = style.pointStyleResolver(
        "40001", "110100", fiveAstral, 10.0, 1.0f);
    ASSERT_TRUE(retinaAstral.labelLayout);
    ASSERT_TRUE(oneXAstral.labelLayout);
    EXPECT_FLOAT_EQ(60.0f, retinaAstral.labelLayout->iconWidthPx);
    EXPECT_FLOAT_EQ(60.0f * 9.0f / 7.0f,
                    oneXAstral.labelLayout->iconWidthPx);
}

TEST(AmapClassicLabelStyleTest, EveryOfficialIconFrameHasAValidUniqueCell) {
    ASSERT_TRUE(amapClassicPoiIconContractsValid());
    const auto atlases = amapClassicPoiIconAtlases();
    EXPECT_EQ((std::vector<int>{1, 4, 7, 8, 9, 11, 23, 24, 26, 50, 51,
                                53, 64, 69, 71, 103, 106}), atlases);
    for (int atlas : atlases) {
        const auto frames = amapClassicPoiIconFrames(atlas);
        ASSERT_FALSE(frames.empty()) << atlas;
        std::vector<int> indices;
        for (const auto& frame : frames) {
            EXPECT_EQ(atlas, frame.atlas);
            EXPECT_GT(frame.iconIndex, 0);
            EXPECT_GT(frame.cellWidth, 0);
            EXPECT_GT(frame.cellHeight, 0);
            EXPECT_GT(frame.atlasWidth, 0);
            EXPECT_GT(frame.atlasHeight, 0);
            const int columns = frame.atlasWidth / frame.cellWidth;
            ASSERT_GT(columns, 0);
            const int zeroBased = frame.iconIndex - 1;
            const int x0 = (zeroBased % columns) * frame.cellWidth;
            const int y0 = (zeroBased / columns) * frame.cellHeight;
            EXPECT_LE(x0 + frame.cellWidth, frame.atlasWidth)
                << atlas << ':' << frame.iconIndex;
            EXPECT_LE(y0 + frame.cellHeight, frame.atlasHeight)
                << atlas << ':' << frame.iconIndex;
            indices.push_back(frame.iconIndex);
        }
        EXPECT_TRUE(std::is_sorted(indices.begin(), indices.end()));
        EXPECT_EQ(indices.end(), std::unique(indices.begin(), indices.end()));
    }
}

TEST(AmapClassicLabelStyleTest, CanCoveredUsesOfficialFlagsBitOne) {
    EXPECT_TRUE(amapClassicPoiCanCovered(10002, 32, 3.0));
    EXPECT_TRUE(amapClassicPoiCanCovered(10002, 32, 4.0));
    EXPECT_FALSE(amapClassicPoiCanCovered(10002, 32, 5.0));
    EXPECT_TRUE(amapClassicPoiCanCovered(10002, 34, 6.0));
    EXPECT_FALSE(amapClassicPoiCanCovered(12024, 34, 6.0));
}

TEST(AmapClassicLabelStyleTest, Y8tBitFourHasNoDownstreamRenderPayload) {
    EXPECT_NE(0, amapClassicPoiFlags(12024, 1356, 10.0) & 4);
    EXPECT_NE(0, amapClassicPoiFlags(10002, 32, 4.0) & 4);
    // The fixed runtime only decodes/forwards Y8t and has no placement,
    // geometry, SDF, VBO or draw consumer. ResolvedPointStyle intentionally
    // exposes no bit-4 state; only observable bit 1 may cross this boundary.
}

TEST(AmapClassicLabelStyleTest, PoiLayoutUsesOnlyOfficialLateBoundContract) {
    FeatureRenderStyle style;
    style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    ASSERT_TRUE(style.pointStyleResolver);

    const auto ordinary =
        style.pointStyleResolver("12024", "1356", "车站", 10.0, 2.0f);
    ASSERT_TRUE(ordinary.labelLayout);
    EXPECT_EQ(FeatureRenderStyle::LabelDirection::Bottom,
              ordinary.labelLayout->direction);
    EXPECT_FLOAT_EQ(0.0f, ordinary.labelLayout->offsetXPx);
    EXPECT_FLOAT_EQ(0.0f, ordinary.labelLayout->offsetYPx);
    EXPECT_FLOAT_EQ(21.0f, ordinary.labelLayout->iconWidthPx);
    EXPECT_FLOAT_EQ(10.0f, ordinary.labelLayout->iconAnchorXPx);
    EXPECT_FLOAT_EQ(10.0f, ordinary.labelLayout->iconAnchorYPx);
    EXPECT_EQ(FeatureRenderStyle::ProviderLabelLayout::IconAnchor::NumericTopLeft,
              ordinary.labelLayout->iconAnchor);

    const auto atlasNine =
        style.pointStyleResolver("10007", "190", "景区", 10.0, 2.0f);
    ASSERT_TRUE(atlasNine.labelLayout);
    ASSERT_TRUE(atlasNine.enabled);
    EXPECT_EQ(9, atlasNine.officialIconAtlas);
    EXPECT_FLOAT_EQ(48.0f, atlasNine.labelLayout->iconWidthPx);
    EXPECT_FLOAT_EQ(48.0f, atlasNine.labelLayout->iconHeightPx);
    EXPECT_EQ(FeatureRenderStyle::ProviderLabelLayout::IconAnchor::BottomCenter,
              atlasNine.labelLayout->iconAnchor);

    const auto dynamic =
        style.pointStyleResolver("12024", "1230", "测试背景", 20.0, 2.0f);
    ASSERT_TRUE(dynamic.labelLayout);
    EXPECT_FALSE(dynamic.enabled)
        << "q8t must not re-enter the fixed point-icon path";
    EXPECT_EQ("amap-icons-4-73",
              dynamic.labelLayout->dynamicBackgroundImage);
    EXPECT_EQ(FeatureRenderStyle::LabelDirection::Center,
              dynamic.labelLayout->direction);
    EXPECT_FLOAT_EQ(0.0f, dynamic.labelLayout->offsetXPx);
    EXPECT_FLOAT_EQ(-1.0f, dynamic.labelLayout->offsetYPx);

    const auto capital =
        style.pointStyleResolver("10002", "5", "北京市", 10.0, 2.0f);
    const auto sameIdentityDifferentName =
        style.pointStyleResolver("10002", "5", "澳门", 10.0, 2.0f);
    ASSERT_TRUE(capital.labelLayout);
    ASSERT_TRUE(sameIdentityDifferentName.labelLayout);
    EXPECT_EQ(FeatureRenderStyle::LabelDirection::Top,
              capital.labelLayout->direction);
    EXPECT_FLOAT_EQ(3.0f, capital.labelLayout->offsetYPx);
    EXPECT_EQ(FeatureRenderStyle::LabelDirection::Left,
              sameIdentityDifferentName.labelLayout->direction);
    EXPECT_FLOAT_EQ(-2.0f,
                    sameIdentityDifferentName.labelLayout->offsetYPx);
    ASSERT_TRUE(capital.minZoom);
    ASSERT_TRUE(capital.maxZoom);
    EXPECT_DOUBLE_EQ(3.8, *capital.minZoom);
    EXPECT_DOUBLE_EQ(4.7, *capital.maxZoom);

    const auto labelOnly =
        style.pointStyleResolver("12024", "1241", "文本", 10.0, 2.0f);
    ASSERT_TRUE(labelOnly.labelLayout);
    EXPECT_FALSE(labelOnly.enabled);
    EXPECT_EQ(FeatureRenderStyle::LabelDirection::Center,
              labelOnly.labelLayout->direction);
    EXPECT_FLOAT_EQ(0.0f, labelOnly.labelLayout->offsetXPx);
    EXPECT_FLOAT_EQ(0.0f, labelOnly.labelLayout->offsetYPx);

    const auto invalid =
        style.pointStyleResolver("bad", "1241", "未知", 10.0, 2.0f);
    EXPECT_FALSE(invalid.labelLayout);
    EXPECT_FALSE(invalid.enabled);
}

TEST(AmapClassicLabelStyleTest,
     EveryOfficialIdentityAndDisplayZoomUsesOneResolverContract) {
    FeatureRenderStyle style = earth_engine::testing::amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Poi);
    ASSERT_TRUE(style.pointStyleResolver);
    const auto identities = amapClassicPoiIdentities();
    ASSERT_EQ(2186u, identities.size());
    for (const auto& [classCode, subKey] : identities) {
        const std::string cls = std::to_string(classCode);
        const std::string sub = std::to_string(subKey);
        for (int displayZoom = 0; displayZoom < 30; ++displayZoom) {
            const auto fixed = resolveAmapClassicPoiIconStyle(
                classCode, subKey, displayZoom);
            const auto dynamic = resolveAmapClassicPoiDynamicBackgroundStyle(
                classCode, subKey, displayZoom);
            const auto resolved = style.pointStyleResolver(
                cls, sub, "普通名称", displayZoom, 2.0f);
            ASSERT_TRUE(resolved.labelLayout)
                << classCode << ':' << subKey << " z=" << displayZoom;
            EXPECT_EQ(fixed.enabled, resolved.enabled);
            EXPECT_EQ(fixed.frameName(), resolved.image);
            EXPECT_EQ(fixed.atlas, resolved.officialIconAtlas);
            EXPECT_FLOAT_EQ(fixed.displayHeightPx, resolved.sizePx);
            EXPECT_EQ(dynamic.enabled,
                      !resolved.labelLayout->dynamicBackgroundImage.empty());
            EXPECT_EQ(dynamic.atlas,
                      resolved.officialDynamicBackgroundAtlas);
            if (dynamic.enabled) {
                EXPECT_FALSE(fixed.enabled);
                EXPECT_EQ(dynamic.frameName(),
                          resolved.labelLayout->dynamicBackgroundImage);
                EXPECT_FLOAT_EQ(dynamic.anchorXPx,
                                resolved.labelLayout->iconAnchorXPx);
                EXPECT_FLOAT_EQ(dynamic.anchorYPx,
                                resolved.labelLayout->iconAnchorYPx);
                EXPECT_EQ(FeatureRenderStyle::LabelDirection::Center,
                          resolved.labelLayout->direction);
                EXPECT_FLOAT_EQ(0.0f, resolved.labelLayout->offsetXPx);
                EXPECT_FLOAT_EQ(-1.0f, resolved.labelLayout->offsetYPx);
            } else if (fixed.enabled) {
                if (classCode == 40001) {
                    EXPECT_FLOAT_EQ(24.0f,
                                    resolved.labelLayout->iconWidthPx);
                    EXPECT_FLOAT_EQ(24.0f,
                                    resolved.labelLayout->iconHeightPx);
                    EXPECT_FLOAT_EQ(12.0f,
                                    resolved.labelLayout->iconAnchorXPx);
                    EXPECT_FLOAT_EQ(12.0f,
                                    resolved.labelLayout->iconAnchorYPx);
                    EXPECT_EQ(FeatureRenderStyle::LabelDirection::Center,
                              resolved.labelLayout->direction);
                } else {
                    EXPECT_FLOAT_EQ(fixed.displayWidthPx,
                                    resolved.labelLayout->iconWidthPx);
                    EXPECT_FLOAT_EQ(fixed.displayHeightPx,
                                    resolved.labelLayout->iconHeightPx);
                    EXPECT_FLOAT_EQ(fixed.anchorXPx,
                                    resolved.labelLayout->iconAnchorXPx);
                    EXPECT_FLOAT_EQ(fixed.anchorYPx,
                                    resolved.labelLayout->iconAnchorYPx);
                    EXPECT_EQ(fixed.anchor,
                              resolved.labelLayout->iconAnchor);
                    EXPECT_EQ(FeatureRenderStyle::LabelDirection::Bottom,
                              resolved.labelLayout->direction);
                }
            } else {
                EXPECT_EQ(FeatureRenderStyle::LabelDirection::Center,
                          resolved.labelLayout->direction);
            }
            const int flags = amapClassicPoiFlags(
                classCode, subKey, displayZoom);
            EXPECT_EQ((flags & 1) != 0, resolved.officialCanCovered);
        }
    }
}

TEST(AmapClassicLabelStyleTest,
     EveryOfficialLabelRecordMatchesInstalledExpressionsAtEveryZoom) {
    const FeatureRenderStyle style =
        earth_engine::testing::amapOfficialStyleForTest(
            FeatureRenderLayer::AmapClassicProfile::Poi);
    const auto records = amapClassicPoiLabelRecordsForTest();
    ASSERT_EQ(2213u, records.size());

    std::map<int, std::vector<AmapClassicPoiLabelRecordForTest>> grouped;
    for (const auto& row : records) {
        grouped[amapClassicLabelIdentity(row.classCode, row.subKey)]
            .push_back(row);
    }
    ASSERT_EQ(2003u, grouped.size());

    const auto colorAt = [](const auto& table, int identity, double zoom) {
        const auto it = table.find(identity);
        if (it == table.end() || !it->second)
            return std::optional<std::array<float, 4>>{};
        const auto value = it->second->evaluate(nullptr, zoom);
        if (!value || value->kind() != StyleValue::Kind::Color)
            return std::optional<std::array<float, 4>>{};
        return std::optional<std::array<float, 4>>{value->color()};
    };
    for (const auto& [identity, rows] : grouped) {
        for (int displayZoom = 0; displayZoom < 30; ++displayZoom) {
            const int providerZoom = displayZoom + 1;
            const auto expected = std::find_if(
                rows.begin(), rows.end(), [&](const auto& row) {
                    return providerZoom >= row.minZoom &&
                           providerZoom <= row.maxZoom;
                });
            const bool visible = expected != rows.end();
            const auto minIt = style.labelMinZoomByStyleGroup.find(identity);
            const auto maxIt = style.labelMaxZoomByStyleGroup.find(identity);
            const bool installedVisible =
                minIt != style.labelMinZoomByStyleGroup.end() &&
                maxIt != style.labelMaxZoomByStyleGroup.end() &&
                displayZoom >= minIt->second && displayZoom <= maxIt->second;
            EXPECT_EQ(visible,
                      installedVisible &&
                          FeatureRenderLayer::labelStyleVisibleAtZoom(
                              style, identity, displayZoom))
                << "identity=" << identity << " z=" << displayZoom;
            if (!visible) continue;
            EXPECT_FLOAT_EQ(expected->sizePx,
                            FeatureRenderLayer::resolvedLabelSizePx(
                                style, identity, displayZoom, 0.0f))
                << "identity=" << identity << " z=" << displayZoom;
            EXPECT_EQ(expected->color,
                      colorAt(style.labelColorExprByStyleGroup,
                              identity, displayZoom))
                << "identity=" << identity << " z=" << displayZoom;
            EXPECT_EQ(expected->haloColor,
                      colorAt(style.labelHaloColorExprByStyleGroup,
                              identity, displayZoom))
                << "identity=" << identity << " z=" << displayZoom;
        }
    }
}
