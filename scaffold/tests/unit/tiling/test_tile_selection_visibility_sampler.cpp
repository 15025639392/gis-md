#include <gtest/gtest.h>

#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileSelectionVisibilitySampler.h"
#include "earth_engine/tiling/TilesetTile.h"

#include <vector>

using namespace earth_engine;

namespace {

const TileSelectionVisibilityContext cameraContext{true, 0.0, 0.0};
const std::vector<SelectorView> noViews;

} // namespace

TEST(
    TileSelectionVisibilitySamplerTest,
    RenderUnderCameraMakesTileVisibleWithoutFrustum) {
    TilesetTile tile(
        TileKey{"test", 0, 0, 0},
        Rectangle{-0.25, -0.25, 0.25, 0.25});

    const TileSelectionVisibilitySample sample =
        TileSelectionVisibilitySampler::sampleTileBounds(
            tile,
            noViews,
            cameraContext);

    EXPECT_TRUE(sample.visibleFromCamera);
    EXPECT_FALSE(sample.inFrustum);
    EXPECT_TRUE(sample.cameraInside);
}

TEST(
    TileSelectionVisibilitySamplerTest,
    DisabledRenderUnderCameraRequiresFrustumVisibility) {
    TilesetTile tile(
        TileKey{"test", 0, 0, 0},
        Rectangle{-0.25, -0.25, 0.25, 0.25});

    const TileSelectionVisibilitySample sample =
        TileSelectionVisibilitySampler::sampleTileBounds(
            tile,
            noViews,
            TileSelectionVisibilityContext{false, 0.0, 0.0});

    EXPECT_FALSE(sample.visibleFromCamera);
    EXPECT_FALSE(sample.inFrustum);
    EXPECT_TRUE(sample.cameraInside);
}

TEST(
    TileSelectionVisibilitySamplerTest,
    ChildBoundsUseVisibleChild) {
    TilesetTile parent(
        TileKey{"test", 0, 0, 0},
        Rectangle{-1.0, -1.0, 1.0, 1.0});
    TilesetTile outsideChild(
        TileKey{"test", 1, 0, 0},
        Rectangle{2.0, 2.0, 3.0, 3.0},
        &parent);
    TilesetTile cameraChild(
        TileKey{"test", 1, 1, 0},
        Rectangle{-0.25, -0.25, 0.25, 0.25},
        &parent);
    parent.children.push_back(&outsideChild);
    parent.children.push_back(&cameraChild);

    const TileSelectionVisibilitySample sample =
        TileSelectionVisibilitySampler::sampleChildBounds(
            parent.children,
            noViews,
            cameraContext);

    EXPECT_TRUE(sample.visibleFromCamera);
    EXPECT_FALSE(sample.inFrustum);
    EXPECT_TRUE(sample.cameraInside);
}

TEST(
    TileSelectionVisibilitySamplerTest,
    ReplaceParentSamplesChildBounds) {
    TilesetTile parent(
        TileKey{"test", 0, 0, 0},
        Rectangle{2.0, 2.0, 3.0, 3.0});
    TilesetTile visibleChild(
        TileKey{"test", 1, 0, 0},
        Rectangle{-0.25, -0.25, 0.25, 0.25},
        &parent);
    parent.refine = TileRefine::Replace;
    parent.children.push_back(&visibleChild);

    const TileSelectionVisibilitySample sample =
        TileSelectionVisibilitySampler::sampleForTileSelection(
            parent,
            noViews,
            cameraContext);

    EXPECT_TRUE(sample.visibleFromCamera);
    EXPECT_FALSE(sample.cameraInside);
}

TEST(TileSelectionVisibilitySamplerTest, AddParentSamplesOwnBounds) {
    TilesetTile parent(
        TileKey{"test", 0, 0, 1},
        Rectangle{-0.25, -0.25, 0.25, 0.25});
    TilesetTile outsideChild(
        TileKey{"test", 1, 1, 0},
        Rectangle{2.0, 2.0, 3.0, 3.0},
        &parent);
    parent.refine = TileRefine::Add;
    parent.children.push_back(&outsideChild);

    const TileSelectionVisibilitySample sample =
        TileSelectionVisibilitySampler::sampleForTileSelection(
            parent,
            noViews,
            cameraContext);

    EXPECT_TRUE(sample.visibleFromCamera);
    EXPECT_TRUE(sample.cameraInside);
}

TEST(
    TileSelectionVisibilitySamplerTest,
    UnconditionalChildKeepsReplaceParentBounds) {
    TilesetTile parent(
        TileKey{"test", 0, 1, 0},
        Rectangle{-0.25, -0.25, 0.25, 0.25});
    TilesetTile outsideChild(
        TileKey{"test", 1, 0, 1},
        Rectangle{2.0, 2.0, 3.0, 3.0},
        &parent);
    outsideChild.unconditionallyRefine = true;
    parent.refine = TileRefine::Replace;
    parent.children.push_back(&outsideChild);

    const TileSelectionVisibilitySample sample =
        TileSelectionVisibilitySampler::sampleForTileSelection(
            parent,
            noViews,
            cameraContext);

    EXPECT_TRUE(sample.visibleFromCamera);
    EXPECT_TRUE(sample.cameraInside);
}

TEST(
    TileSelectionVisibilitySamplerTest,
    ExplicitBoundingVolumeControlsCameraInsideSelectionBounds) {
    TilesetTile tile(
        TileKey{"test", 0, 2, 0},
        Rectangle{2.0, 2.0, 3.0, 3.0});
    tile.boundingVolume =
        TileBoundingVolume::fromRegion(
            Rectangle{-0.25, -0.25, 0.25, 0.25},
            0.0,
            0.0);

    const TileSelectionVisibilitySample sample =
        TileSelectionVisibilitySampler::sampleTileBounds(
            tile,
            noViews,
            cameraContext);

    EXPECT_TRUE(sample.visibleFromCamera);
    EXPECT_FALSE(sample.inFrustum);
    EXPECT_TRUE(sample.cameraInside);
    EXPECT_TRUE(TileSelectionVisibilitySampler::cameraInsideSelectionBounds(
        tile,
        cameraContext));
}
