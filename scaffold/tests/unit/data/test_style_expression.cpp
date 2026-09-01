#include <gtest/gtest.h>

#include "earth_engine/data/StyleExpression.h"

#include <cmath>

using namespace earth_engine;
using Expr = StyleExpression;

namespace {
const std::array<float, 4> kRed{1, 0, 0, 1};
const std::array<float, 4> kBlue{0, 0, 1, 1};
const double kNaN = std::nan("");
} // namespace

TEST(StyleExpressionTest, LiteralNumberAndColor) {
    const auto n = Expr::literal(42.0)->evaluate(nullptr, kNaN);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(StyleValue::Kind::Number, n->kind());
    EXPECT_DOUBLE_EQ(42.0, n->number());

    const auto c = Expr::literal(kRed)->evaluate(nullptr, kNaN);
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(StyleValue::Kind::Color, c->kind());
    EXPECT_FLOAT_EQ(1.0f, c->color()[0]);
}

TEST(StyleExpressionTest, GetParsesNumericProperty) {
    Expr::PropertyMap props{{"height", "12.5"}, {"name", "abc"}};
    const auto v = Expr::get("height")->evaluate(&props, kNaN);
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(12.5, v->number());

    // 非数值属性 / 缺属性 / 无上下文 → 求值失败(调用方回落字面量)。
    EXPECT_FALSE(Expr::get("name")->evaluate(&props, kNaN).has_value());
    EXPECT_FALSE(Expr::get("missing")->evaluate(&props, kNaN).has_value());
    EXPECT_FALSE(Expr::get("height")->evaluate(nullptr, kNaN).has_value());
}

TEST(StyleExpressionTest, ZoomRequiresContext) {
    const auto v = Expr::zoom()->evaluate(nullptr, 14.5);
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(14.5, v->number());
    EXPECT_FALSE(Expr::zoom()->evaluate(nullptr, kNaN).has_value());
}

TEST(StyleExpressionTest, MatchSelectsCaseAndFallback) {
    const auto expr = Expr::match(
        "kind",
        {{"tower", Expr::literal(kRed)}, {"gate", Expr::literal(kBlue)}},
        Expr::literal(std::array<float, 4>{0, 1, 0, 1}));

    Expr::PropertyMap tower{{"kind", "tower"}};
    Expr::PropertyMap other{{"kind", "fence"}};
    Expr::PropertyMap empty;

    EXPECT_FLOAT_EQ(1.0f, expr->evaluate(&tower, kNaN)->color()[0]);
    EXPECT_FLOAT_EQ(1.0f, expr->evaluate(&other, kNaN)->color()[1]);  // 兜底
    EXPECT_FLOAT_EQ(1.0f, expr->evaluate(&empty, kNaN)->color()[1]);  // 缺属性兜底
}

TEST(StyleExpressionTest, InterpolateNumberClampsAndLerps) {
    const auto expr = Expr::interpolateLinear(
        Expr::zoom(),
        {{10.0, Expr::literal(2.0)}, {14.0, Expr::literal(10.0)}});

    EXPECT_DOUBLE_EQ(2.0, expr->evaluate(nullptr, 8.0)->number());    // 下钳
    EXPECT_DOUBLE_EQ(10.0, expr->evaluate(nullptr, 20.0)->number());  // 上钳
    EXPECT_DOUBLE_EQ(6.0, expr->evaluate(nullptr, 12.0)->number());   // 中点
    EXPECT_FALSE(expr->evaluate(nullptr, kNaN).has_value());  // 无 zoom
}

TEST(StyleExpressionTest, InterpolateColorLerps) {
    const auto expr = Expr::interpolateLinear(
        Expr::zoom(),
        {{0.0, Expr::literal(kRed)}, {10.0, Expr::literal(kBlue)}});
    const auto mid = expr->evaluate(nullptr, 5.0);
    ASSERT_TRUE(mid.has_value());
    EXPECT_FLOAT_EQ(0.5f, mid->color()[0]);
    EXPECT_FLOAT_EQ(0.5f, mid->color()[2]);
}

TEST(StyleExpressionTest, InterpolateMixedStopKindsFails) {
    const auto expr = Expr::interpolateLinear(
        Expr::zoom(),
        {{0.0, Expr::literal(1.0)}, {10.0, Expr::literal(kBlue)}});
    EXPECT_FALSE(expr->evaluate(nullptr, 5.0).has_value());
}

TEST(StyleExpressionTest, StepUsesHardStopsAndClamps) {
    const auto expr = Expr::step(
        Expr::zoom(),
        {{6.0, Expr::literal(kRed)}, {8.0, Expr::literal(kBlue)},
         {10.0, Expr::literal(std::array<float, 4>{0, 1, 0, 1})}});
    EXPECT_EQ(kRed, expr->evaluate(nullptr, 3.0)->color());
    EXPECT_EQ(kRed, expr->evaluate(nullptr, 7.999)->color());
    EXPECT_EQ(kBlue, expr->evaluate(nullptr, 8.0)->color());
    EXPECT_EQ(kBlue, expr->evaluate(nullptr, 9.999)->color());
    EXPECT_FLOAT_EQ(1.0f, expr->evaluate(nullptr, 20.0)->color()[1]);
    EXPECT_FALSE(expr->evaluate(nullptr, kNaN).has_value());
}

TEST(StyleExpressionTest, DiscreteZoomSwitchesAtConfiguredFraction) {
    const auto expr = StyleExpression::discreteZoom(0.8);
    ASSERT_TRUE(expr->referencesZoom());
    EXPECT_DOUBLE_EQ(9.0, expr->evaluate(nullptr, 9.79)->number());
    EXPECT_DOUBLE_EQ(10.0, expr->evaluate(nullptr, 9.80)->number());
    EXPECT_DOUBLE_EQ(10.0, expr->evaluate(nullptr, 10.0)->number());
    EXPECT_DOUBLE_EQ(-2.0, expr->evaluate(nullptr, -1.21)->number());
    EXPECT_DOUBLE_EQ(-1.0, expr->evaluate(nullptr, -1.20)->number());
}

TEST(StyleExpressionTest, ReferenceFlags) {
    EXPECT_FALSE(Expr::literal(1.0)->referencesProperties());
    EXPECT_FALSE(Expr::literal(1.0)->referencesZoom());

    EXPECT_TRUE(Expr::get("h")->referencesProperties());
    EXPECT_FALSE(Expr::get("h")->referencesZoom());

    EXPECT_TRUE(Expr::zoom()->referencesZoom());
    EXPECT_FALSE(Expr::zoom()->referencesProperties());

    // Match 分支选择本身依赖属性;分支里的 zoom 也要被察觉。
    const auto m = Expr::match("kind", {{"a", Expr::zoom()}},
                               Expr::literal(1.0));
    EXPECT_TRUE(m->referencesProperties());
    EXPECT_TRUE(m->referencesZoom());

    const auto interp = Expr::interpolateLinear(
        Expr::zoom(), {{0.0, Expr::literal(1.0)}});
    EXPECT_TRUE(interp->referencesZoom());
    EXPECT_FALSE(interp->referencesProperties());

    const auto dataInterp = Expr::interpolateLinear(
        Expr::get("h"), {{0.0, Expr::literal(1.0)}});
    EXPECT_TRUE(dataInterp->referencesProperties());
    EXPECT_FALSE(dataInterp->referencesZoom());

    const auto zoomStep = Expr::step(
        Expr::zoom(), {{0.0, Expr::literal(1.0)}});
    EXPECT_TRUE(zoomStep->referencesZoom());
    EXPECT_FALSE(zoomStep->referencesProperties());
}

TEST(StyleExpressionTest, NestedMatchIntoInterpolate) {
    // 组合:按 kind 选不同的 zoom 曲线(数值)。
    const auto expr = Expr::match(
        "kind",
        {{"major",
          Expr::interpolateLinear(Expr::zoom(), {{10.0, Expr::literal(8.0)},
                                                 {14.0, Expr::literal(16.0)}})}},
        Expr::literal(4.0));
    Expr::PropertyMap major{{"kind", "major"}};
    Expr::PropertyMap minor{{"kind", "minor"}};
    EXPECT_DOUBLE_EQ(12.0, expr->evaluate(&major, 12.0)->number());
    EXPECT_DOUBLE_EQ(4.0, expr->evaluate(&minor, 12.0)->number());
}
