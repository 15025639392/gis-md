#include <gtest/gtest.h>

#include "earth_engine/tiling/TileBoundingVolume.h"
#include "earth_engine/tiling/TileViewerRequestVolumePolicy.h"

#include <optional>
#include <vector>

using namespace earth_engine;

TEST(TileViewerRequestVolumePolicyTest, MissingVolumeAllowsSelection) {
    const std::optional<TileBoundingVolume> noVolume;
    std::vector<SelectorView> noViews;

    EXPECT_FALSE(TileViewerRequestVolumePolicy::hasRequestVolume(noVolume));
    EXPECT_TRUE(TileViewerRequestVolumePolicy::allowsAnyView(
        noVolume,
        noViews));
}

TEST(TileViewerRequestVolumePolicyTest, SphereVolumeContainsCameraPosition) {
    const TileBoundingVolume sphere =
        TileBoundingVolume::fromSphere(Vec3(10.0, 0.0, 0.0), 5.0);

    EXPECT_TRUE(TileViewerRequestVolumePolicy::containsPosition(
        sphere,
        Vec3(12.0, 0.0, 0.0)));
    EXPECT_FALSE(TileViewerRequestVolumePolicy::containsPosition(
        sphere,
        Vec3(20.1, 0.0, 0.0)));
}

TEST(TileViewerRequestVolumePolicyTest, BoxBoundaryIsNotToleranceInflated) {
    const TileBoundingVolume box =
        TileBoundingVolume::fromBox(
            Vec3::zero(),
            Vec3::unitX(),
            Vec3::unitY(),
            Vec3::unitZ());

    EXPECT_TRUE(TileViewerRequestVolumePolicy::containsPosition(
        box,
        Vec3(1.0, 0.0, 0.0)));
    EXPECT_FALSE(TileViewerRequestVolumePolicy::containsPosition(
        box,
        Vec3(1.0 + 1e-12, 0.0, 0.0)));
}

TEST(TileViewerRequestVolumePolicyTest, PresentVolumeRequiresAnyContainedView) {
    const std::optional<TileBoundingVolume> volume =
        TileBoundingVolume::fromSphere(Vec3(10.0, 0.0, 0.0), 5.0);

    std::vector<SelectorView> views(2);
    views[0].position = Vec3(20.1, 0.0, 0.0);
    views[1].position = Vec3(12.0, 0.0, 0.0);
    EXPECT_TRUE(TileViewerRequestVolumePolicy::allowsAnyView(volume, views));

    views[1].position = Vec3(30.0, 0.0, 0.0);
    EXPECT_FALSE(TileViewerRequestVolumePolicy::allowsAnyView(volume, views));

    views.clear();
    EXPECT_FALSE(TileViewerRequestVolumePolicy::allowsAnyView(volume, views));
}

