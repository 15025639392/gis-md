#include <gtest/gtest.h>

#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileBoundingVolume.h"
#include "earth_engine/tiling/TileMotionCullPolicy.h"
#include "earth_engine/tiling/TilesetTile.h"

#include <cmath>

using namespace earth_engine;

namespace {

// movementRatio = multiplier × delta / max(2r, 1);  defer 当 ratio >= 1.0。
// 用 multiplier=60、radius=100 → diameter=200 → 临界 delta = 200/60 ≈ 3.333m。

TileMotionCullPolicy::Input makeInput(double delta, double radius) {
    TileMotionCullPolicy::Input in;
    in.cullRequestsWhileMoving = true;
    in.cullRequestsWhileMovingMultiplier = 60.0;
    in.cameraPositionDeltaMagnitude = delta;
    in.boundingSphereRadius = radius;
    return in;
}

} // namespace

TEST(TileMotionCullPolicy, DisabledFlagNeverDefers) {
    TileMotionCullPolicy::Input in = makeInput(1000.0, 100.0);
    in.cullRequestsWhileMoving = false;
    EXPECT_FALSE(TileMotionCullPolicy::shouldDeferForMotion(in));
}

TEST(TileMotionCullPolicy, StationaryCameraNeverDefers) {
    // delta = 0 → movementRatio = 0 < 1 → 不剔除(相机静止全部加载)。
    EXPECT_FALSE(
        TileMotionCullPolicy::shouldDeferForMotion(makeInput(0.0, 100.0)));
}

TEST(TileMotionCullPolicy, FastMotionSmallTileDefers) {
    // delta 远超临界(3.33m)→ 剔除。
    EXPECT_TRUE(
        TileMotionCullPolicy::shouldDeferForMotion(makeInput(50.0, 100.0)));
}

TEST(TileMotionCullPolicy, SlowMotionBelowThresholdDoesNotDefer) {
    // delta=3m < 临界 3.33m → movementRatio=0.9 < 1 → 不剔除。
    EXPECT_FALSE(
        TileMotionCullPolicy::shouldDeferForMotion(makeInput(3.0, 100.0)));
}

TEST(TileMotionCullPolicy, ThresholdBoundaryDefersAtEqualOne) {
    // 精确构造 movementRatio == 1.0:delta = diameter / multiplier。
    const double radius = 100.0;
    const double diameter = radius * 2.0;
    const double delta = diameter / 60.0;  // ratio == 1.0 → defer(>=)。
    EXPECT_TRUE(
        TileMotionCullPolicy::shouldDeferForMotion(makeInput(delta, radius)));
}

TEST(TileMotionCullPolicy, LargeTileResistsCullingMoreThanSmall) {
    // 同样的相机位移下,大瓦片(直径大)movementRatio 更小,更不易被剔除。
    const double delta = 20.0;
    EXPECT_TRUE(TileMotionCullPolicy::shouldDeferForMotion(
        makeInput(delta, 50.0)));  // 小瓦片:60×20/100=12 ≥1
    EXPECT_FALSE(TileMotionCullPolicy::shouldDeferForMotion(
        makeInput(delta, 100000.0)));  // 大瓦片:60×20/200000≈0.006 <1
}

TEST(TileMotionCullPolicy, ZeroRadiusNeverDefers) {
    // 无有效包围体半径 → 不剔除(避免 diameter 退化恒剔除的误判)。
    EXPECT_FALSE(
        TileMotionCullPolicy::shouldDeferForMotion(makeInput(1000.0, 0.0)));
}

TEST(TileMotionCullPolicy, BoundingSphereRadiusFromSphereVolume) {
    TilesetTile tile;
    tile.boundingVolume =
        TileBoundingVolume::fromSphere(Vec3(1.0, 2.0, 3.0), 250.0);
    EXPECT_NEAR(TileMotionCullPolicy::boundingSphereRadius(tile), 250.0, 1e-9);
}

TEST(TileMotionCullPolicy, BoundingSphereRadiusFromBoxVolume) {
    // 正交半轴 (100,0,0),(0,200,0),(0,0,300) → 半径 = |a0+a1+a2|
    // = sqrt(100^2+200^2+300^2)。
    TilesetTile tile;
    tile.boundingVolume = TileBoundingVolume::fromBox(
        Vec3::zero(),
        Vec3(100.0, 0.0, 0.0),
        Vec3(0.0, 200.0, 0.0),
        Vec3(0.0, 0.0, 300.0));
    const double expected =
        std::sqrt(100.0 * 100.0 + 200.0 * 200.0 + 300.0 * 300.0);
    EXPECT_NEAR(
        TileMotionCullPolicy::boundingSphereRadius(tile),
        expected,
        1e-6);
}

TEST(TileMotionCullPolicy, BoundingSphereRadiusMissingVolumeIsZero) {
    TilesetTile tile;  // boundingVolume 未设置
    EXPECT_EQ(TileMotionCullPolicy::boundingSphereRadius(tile), 0.0);
}
