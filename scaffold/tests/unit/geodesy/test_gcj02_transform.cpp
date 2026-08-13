#include <gtest/gtest.h>

#include <cmath>

#include "earth_engine/core/geodesy/Gcj02CoordinateTransform.h"

using namespace earth_engine;

namespace {

// 亚毫米(弧度):1e-10 rad ≈ 0.6 mm 地表距离。
constexpr double kTightRad = 1e-10;

double roundtripErrorRad(double lngDeg, double latDeg) {
    const Cartographic wgs = Cartographic::fromDegrees(lngDeg, latDeg);
    const Cartographic gcj = Gcj02CoordinateTransform::fromWgs84(wgs);
    const Cartographic back = Gcj02CoordinateTransform::toWgs84(gcj);
    return std::max(std::abs(back.longitude() - wgs.longitude()),
                    std::abs(back.latitude() - wgs.latitude()));
}

} // namespace

// toWgs84(fromWgs84(p)) == p:两轮不动点迭代在偏移场梯度 ~1e-5 下应收敛到
// 远低于亚毫米。取样覆盖:重庆(demo 场景)、北京、广州、兰州(内陆偏移大)。
TEST(Gcj02TransformTest, RoundtripInsideChinaIsSubMillimetre) {
    EXPECT_LT(roundtripErrorRad(106.55, 29.56), kTightRad);  // 重庆
    EXPECT_LT(roundtripErrorRad(116.39, 39.91), kTightRad);  // 北京
    EXPECT_LT(roundtripErrorRad(113.26, 23.13), kTightRad);  // 广州
    EXPECT_LT(roundtripErrorRad(103.83, 36.06), kTightRad);  // 兰州
}

// 境外恒等:与 fromWgs84 同一判据,逐位不变。
TEST(Gcj02TransformTest, OutsideChinaIsIdentity) {
    const Cartographic tokyo = Cartographic::fromDegrees(139.69, 35.69);
    const Cartographic back = Gcj02CoordinateTransform::toWgs84(tokyo);
    EXPECT_EQ(back.longitude(), tokyo.longitude());
    EXPECT_EQ(back.latitude(), tokyo.latitude());
}

// 偏移量本身要在已知量级(GCJ 偏移在城市区通常 100-700 m):防实现退化成
// 恒等(roundtrip 测试对"两个方向都恒等"是盲的)。
TEST(Gcj02TransformTest, OffsetMagnitudeIsInKnownRange) {
    const Cartographic wgs = Cartographic::fromDegrees(106.55, 29.56);
    const Cartographic gcj = Gcj02CoordinateTransform::fromWgs84(wgs);
    const double dLng = std::abs(gcj.longitude() - wgs.longitude());
    const double dLat = std::abs(gcj.latitude() - wgs.latitude());
    // 100 m ≈ 1.57e-5 rad;700 m ≈ 1.1e-4 rad。
    EXPECT_GT(std::max(dLng, dLat), 1e-6);
    EXPECT_LT(std::max(dLng, dLat), 2e-4);
}
