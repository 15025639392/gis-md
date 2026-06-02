#include <gtest/gtest.h>
#include "earth_engine/style/OverlayStyle.h"

using namespace earth_engine;

// ============================================================
// Color
// ============================================================

TEST(OverlayStyleTest, ColorDefaults) {
    Color c;
    EXPECT_FLOAT_EQ(1.0f, c.r);
    EXPECT_FLOAT_EQ(1.0f, c.g);
    EXPECT_FLOAT_EQ(1.0f, c.b);
    EXPECT_FLOAT_EQ(1.0f, c.a);
}

TEST(OverlayStyleTest, ColorFromHex) {
    auto c = Color::fromHex(0xFF800040);
    EXPECT_FLOAT_EQ(1.0f, c.r);
    EXPECT_NEAR(0.50196f, c.g, 1e-4f);  // 0x80 / 255 ≈ 0.50196
    EXPECT_FLOAT_EQ(0.0f, c.b);
    EXPECT_NEAR(0.25098f, c.a, 1e-4f);  // 0x40 / 255 ≈ 0.25098
}

TEST(OverlayStyleTest, ColorToArray) {
    Color c{0.1f, 0.2f, 0.3f, 0.4f};
    auto arr = c.toArray();
    EXPECT_FLOAT_EQ(0.1f, arr[0]);
    EXPECT_FLOAT_EQ(0.2f, arr[1]);
    EXPECT_FLOAT_EQ(0.3f, arr[2]);
    EXPECT_FLOAT_EQ(0.4f, arr[3]);
}

// ============================================================
// PointStyle defaults
// ============================================================

TEST(OverlayStyleTest, PointStyleDefaults) {
    PointStyle ps;
    EXPECT_EQ(PointStyle::Shape::Circle, ps.shape);
    EXPECT_FLOAT_EQ(6.0f, ps.size);
    EXPECT_TRUE(ps.billboard);
    EXPECT_TRUE(ps.depthTest);
}

// ============================================================
// LineStyle defaults
// ============================================================

TEST(OverlayStyleTest, LineStyleDefaults) {
    LineStyle ls;
    EXPECT_FLOAT_EQ(2.0f, ls.width);
    EXPECT_TRUE(ls.dashArray.empty());
    EXPECT_EQ(LineStyle::Cap::Round, ls.cap);
}

// ============================================================
// PolygonStyle defaults
// ============================================================

TEST(OverlayStyleTest, PolygonStyleDefaults) {
    PolygonStyle ps;
    EXPECT_FLOAT_EQ(0.3f, ps.fillColor.a);
    EXPECT_FLOAT_EQ(1.0f, ps.fillOpacity);
    EXPECT_FLOAT_EQ(1.5f, ps.outlineWidth);
}

// ============================================================
// LayerStyle
// ============================================================

TEST(OverlayStyleTest, LayerStyleDefaults) {
    LayerStyle ls;
    EXPECT_FLOAT_EQ(1.0f, ls.opacity);
    EXPECT_EQ(0, ls.minZoom);
    EXPECT_EQ(20, ls.maxZoom);
    EXPECT_EQ(AltitudeMode::ClampToGround, ls.altitudeMode);
}

// ============================================================
// InteractionStyle
// ============================================================

TEST(OverlayStyleTest, InteractionStyleFind) {
    InteractionStyle is;
    is.states = {
        InteractionStyleOverride("hover", {0.2f, 0.2f, 0.0f, 0.0f}),
        InteractionStyleOverride("selected", {0.0f, 0.4f, 0.4f, 0.0f}),
    };

    auto* hover = is.find("hover");
    ASSERT_NE(nullptr, hover);
    EXPECT_EQ("hover", hover->state);

    auto* missing = is.find("disabled");
    EXPECT_EQ(nullptr, missing);
}

// ============================================================
// Factory functions
// ============================================================

TEST(OverlayStyleTest, MakeDefaultPointStyle) {
    auto s = makeDefaultPointStyle();
    EXPECT_TRUE(std::holds_alternative<PointStyle>(s.layer.geometry));
    EXPECT_EQ(2u, s.interaction.states.size());
    EXPECT_EQ("hover", s.interaction.states[0].state);
    EXPECT_EQ("selected", s.interaction.states[1].state);
}

TEST(OverlayStyleTest, MakeDefaultLineStyle) {
    auto s = makeDefaultLineStyle();
    EXPECT_TRUE(std::holds_alternative<LineStyle>(s.layer.geometry));
    EXPECT_EQ(2u, s.interaction.states.size());
}

TEST(OverlayStyleTest, MakeDefaultPolygonStyle) {
    auto s = makeDefaultPolygonStyle();
    EXPECT_TRUE(std::holds_alternative<PolygonStyle>(s.layer.geometry));
    EXPECT_EQ(2u, s.interaction.states.size());
}
