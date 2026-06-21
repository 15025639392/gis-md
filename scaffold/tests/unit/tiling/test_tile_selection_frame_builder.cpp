#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/tiling/TileSelectionFrameBuilder.h"

using namespace earth_engine;

TEST(TileSelectionFrameBuilderTest, EmptySelectorViewsStayEmpty) {
    const SelectorFrame selectorFrame =
        TileSelectionFrameBuilder::build(
            FrameState{},
            {{100.0, 0.001}});

    EXPECT_TRUE(selectorFrame.views.empty());
    EXPECT_TRUE(selectorFrame.fogDensities.empty());
}

TEST(TileSelectionFrameBuilderTest, CopiesViewsAndComputesFogPerHeight) {
    FrameState frameState;
    SelectorView lowView;
    lowView.position = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic::fromRadians(0.0, 0.0, 50.0));
    lowView.viewportHeightPixels = 720;
    SelectorView highView;
    highView.position = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic::fromRadians(0.0, 0.0, 300.0));
    highView.viewportHeightPixels = 1080;
    frameState.selectorViews = {lowView, highView};

    const SelectorFrame selectorFrame =
        TileSelectionFrameBuilder::build(
            frameState,
            {
                {100.0, 0.001},
                {300.0, 0.005},
            });

    ASSERT_EQ(selectorFrame.views.size(), 2u);
    EXPECT_EQ(selectorFrame.views[0].viewportHeightPixels, 720);
    EXPECT_EQ(selectorFrame.views[1].viewportHeightPixels, 1080);
    ASSERT_EQ(selectorFrame.fogDensities.size(), 2u);
    EXPECT_NEAR(selectorFrame.fogDensities[0], 0.001, 1e-12);
    EXPECT_NEAR(selectorFrame.fogDensities[1], 0.005, 1e-12);
}

TEST(TileSelectionFrameBuilderTest, BelowEllipsoidHeightParticipatesInFog) {
    FrameState frameState;
    SelectorView view;
    view.position = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic::fromRadians(0.0, 0.0, -100.0));
    frameState.selectorViews = {view};

    const SelectorFrame selectorFrame =
        TileSelectionFrameBuilder::build(
            frameState,
            {
                {-200.0, 0.001},
                {100.0, 0.004},
            });

    ASSERT_EQ(selectorFrame.fogDensities.size(), 1u);
    EXPECT_NEAR(selectorFrame.fogDensities[0], 0.002, 1e-12);
}
