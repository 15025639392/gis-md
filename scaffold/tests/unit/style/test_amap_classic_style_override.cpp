#include <gtest/gtest.h>
#include "../../helpers/AmapOfficialStyleTestAdapter.h"

#include "earth_engine/style/AmapClassicStyleOverride.h"

using namespace earth_engine;
using namespace earth_engine::testing;

namespace {
int sg(int classCode, int subKey) { return classCode * 1000 + subKey; }
std::array<float, 4> colorAt(const StyleExpression::Ptr& expr) {
    const auto v = expr->evaluate(nullptr, 0.0);
    EXPECT_TRUE(v.has_value());
    EXPECT_EQ(StyleValue::Kind::Color, v->kind());
    return v->color();
}
double numberAt(const StyleExpression::Ptr& expr) {
    const auto v = expr->evaluate(nullptr, 0.0);
    EXPECT_TRUE(v.has_value());
    EXPECT_EQ(StyleValue::Kind::Number, v->kind());
    return v->number();
}
}  // namespace

// Overrides sit on top of the sealed official contract: replacing a sealed
// surface identity color must not disturb unrelated identities.
TEST(AmapClassicStyleOverride, SurfaceOverrideReplacesSealedFillOnly) {
    FeatureRenderStyle style = amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Main);
    ASSERT_TRUE(style.fillColorExprByStyleGroup.count(sg(30001, 2)));
    const auto before = colorAt(style.fillColorExprByStyleGroup.at(sg(30001, 2)));
    EXPECT_NE(before[0], 0.1f) << "sealed color must differ from override";

    AmapClassicStyleOverrides o;
    o.surface.push_back({30001, 2, {0.1f, 0.2f, 0.3f, 1.0f}});
    applyAmapClassicStyleOverrides(style, o);

    ASSERT_TRUE(style.fillColorExprByStyleGroup.count(sg(30001, 2)));
    const auto c = colorAt(style.fillColorExprByStyleGroup.at(sg(30001, 2)));
    EXPECT_NEAR(c[0], 0.1f, 1e-5);
    EXPECT_NEAR(c[1], 0.2f, 1e-5);
    EXPECT_NEAR(c[2], 0.3f, 1e-5);
    EXPECT_NEAR(c[3], 1.0f, 1e-5);
}

// A line override pins both color and width for that road identity.
TEST(AmapClassicStyleOverride, LineOverrideReplacesColorAndWidth) {
    FeatureRenderStyle style = amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Main);
    AmapClassicStyleOverrides o;
    o.line.push_back({20001, 1, {0.9f, 0.1f, 0.1f, 1.0f}, 3.5f});
    applyAmapClassicStyleOverrides(style, o);

    ASSERT_TRUE(style.lineColorExprByStyleGroup.count(sg(20001, 1)));
    const auto c = colorAt(style.lineColorExprByStyleGroup.at(sg(20001, 1)));
    EXPECT_NEAR(c[0], 0.9f, 1e-5);
    EXPECT_NEAR(c[1], 0.1f, 1e-5);

    ASSERT_TRUE(style.lineWidthExprByStyleGroup.count(sg(20001, 1)));
    EXPECT_NEAR(numberAt(style.lineWidthExprByStyleGroup.at(sg(20001, 1))),
                3.5f, 1e-5);
}

// Empty overrides are a no-op (never clear sealed maps).
TEST(AmapClassicStyleOverride, EmptyOverridesAreNoOp) {
    FeatureRenderStyle style = amapOfficialStyleForTest(
        FeatureRenderLayer::AmapClassicProfile::Main);
    const auto before = style.fillColorExprByStyleGroup.at(sg(30001, 2));
    applyAmapClassicStyleOverrides(style, AmapClassicStyleOverrides{});
    EXPECT_EQ(before, style.fillColorExprByStyleGroup.at(sg(30001, 2)));
}