#include <gtest/gtest.h>

#include "earth_engine/content/EllipsoidTerrainMeshBuilder.h"
#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Gcj02CoordinateTransform.h"
#include "earth_engine/core/math/MathUtils.h"
#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/tiling/RasterOverlayProjection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

using namespace earth_engine;

// =============================================================================
// 「set 1 从存改算」方案的风险验证(第一刀,先于任何 shader 改动;方案 =
// VS 里从 set0 UV + per-tile 参数直接算出影像投影空间 UV,替掉烘焙 set1)。
//
// R1 约定复现:VS 形态的公式——输入**仅** set0 UV 与 per-tile 矩形——能否
//    逐顶点复现烘焙链(rewriteProjectionTexCoords)的 set1。烘焙链经
//    ECEF→cartographic 往返取经纬,参考实现走 rect0+uv0 线性重建,两条路径
//    独立,比对非同义反复。容差压在 unorm16 量化档(1/65535≈1.53e-5)之下:
//    量化是渲染端既有精度地板,参考实现只需别高于它。
// R5 仿射退化:GCJ δ 场用每瓦片 6 系数(中心值+两方向导数)仿射近似的误差,
//    按「瓦片≈屏幕 256px」折算像素。预测:境内 z15 亚 0.1px;z10(瓦片跨度
//    ≈δ 场 sin(6πx) 周期 1/3°,最坏点)~0.5px;跨中国框瓦片是阶跃,仿射
//    原理上代表不了,量化其上界供体验裁决,不假装修复。
//
// 生产链盘点(2026-08-16 核实):烘焙 set1 的活产地只有
// EllipsoidTerrainMeshBuilder::rewriteProjectionTexCoords(heightmap/椭球
// provider 共用);上采样链(GltfTerrainUpsampler)当前生产不可达,见其头注释
// 与 contracts::Gate::ImageryDrivenUpsample —— 若复活,其窗口重归一化的口径
// 依赖同一不变量「UV [0,1] 张满 details.rasterOverlayRectangles[set]」,与本
// 参考实现同源。QM parser 不在本仓。反子午线×WebMercator 归一化矩形的行为
// 本轮未覆盖(GCJ 域距 dateline 远,标准链亦无该组合的既有用例),留待专项。
// =============================================================================

namespace {

// unorm16 量化档:烘焙 UV 上传前会压到这个精度,参考实现的容差地板。
constexpr double kUnorm16Step = 1.0 / 65535.0;

// VS 形态的参考实现(未来产线公式的种子,先以测试形态存在):
// set0(Geographic,NW 约定,归一化于 rect0)→ 经纬 → 投影 → 归一化于 rect1。
std::array<double, 2> vsShapedUv(const std::array<float, 2>& uv0,
                                 const Rectangle& rect0,
                                 const Rectangle& rect1,
                                 RasterOverlayProjection projection) {
    // set0 → 经纬。经度按 rect0 展开(跨反子午线时先展开再回卷,镜像
    // EllipsoidTerrainMeshBuilder::longitudeAt 的约定)。
    double east = rect0.east();
    if (rect0.crossesAntimeridian()) {
        east += MathUtils::TwoPi;
    }
    double lng = rect0.west() +
                 static_cast<double>(uv0[0]) * (east - rect0.west());
    if (lng > MathUtils::OnePi) {
        lng -= MathUtils::TwoPi;
    }
    // NW 约定:v=0 在北缘。
    const double lat = rect0.north() -
                       static_cast<double>(uv0[1]) * rect0.computeHeight();

    const Vec3 projected = projectWorldPositionForRasterOverlay(
        Cartographic::fromRadians(lng, lat), projection);

    // 归一化于 rect1(该投影的 details 矩形),clamp 同烘焙链。
    const double u = std::clamp(
        (projected.x() - rect1.west()) / rect1.width(), 0.0, 1.0);
    const double v = std::clamp(
        (rect1.north() - projected.y()) / rect1.computeHeight(), 0.0, 1.0);
    return {u, v};
}

// 逐顶点比对一条烘焙模型的 set1(含裙墙顶点:它们复制边缘 texcoord,
// 参考实现从其 set0 出发应逐位同路)。返回最大 |Δuv|。
double maxSet1Error(const GltfModel& model,
                    RasterOverlayProjection set1Projection) {
    const GltfPrimitive& primitive = model.primitives.front();
    const auto& details = model.rasterOverlayDetails;
    const Rectangle& rect0 = details.rasterOverlayRectangles[0];
    const Rectangle& rect1 = details.rasterOverlayRectangles[1];
    EXPECT_EQ(details.rasterOverlayProjections[1], set1Projection);
    EXPECT_EQ(primitive.vertexTexCoords[0].size(),
              primitive.vertexTexCoords[1].size());

    double maxErr = 0.0;
    for (size_t i = 0; i < primitive.vertexTexCoords[0].size(); ++i) {
        const std::array<double, 2> ref = vsShapedUv(
            primitive.vertexTexCoords[0][i], rect0, rect1, set1Projection);
        const auto& baked = primitive.vertexTexCoords[1][i];
        maxErr = std::max(
            maxErr,
            std::max(std::abs(ref[0] - static_cast<double>(baked[0])),
                     std::abs(ref[1] - static_cast<double>(baked[1]))));
    }
    return maxErr;
}

// ---- R5:GCJ δ 场的每瓦片仿射近似 ----

struct GcjDelta {
    double dLng = 0.0;
    double dLat = 0.0;
};

GcjDelta gcjDeltaAt(double lngRad, double latRad) {
    const Cartographic wgs = Cartographic::fromRadians(lngRad, latRad);
    const Cartographic shifted = Gcj02CoordinateTransform::fromWgs84(wgs);
    return {shifted.longitude() - wgs.longitude(),
            shifted.latitude() - wgs.latitude()};
}

// 每瓦片 6 系数:中心 δ + 对 u/v 的中心差分导数(u/v 为瓦片内归一化坐标)。
// 这是 VS 端每实例下发的全部数据。
struct GcjAffine {
    GcjDelta center;
    GcjDelta ddu;  // ∂δ/∂u(u 从西到东)
    GcjDelta ddv;  // ∂δ/∂v(v 从北到南,NW 约定)

    static GcjAffine fit(const Rectangle& bounds) {
        const double cLng = 0.5 * (bounds.west() + bounds.east());
        const double cLat = 0.5 * (bounds.south() + bounds.north());
        const double hw = 0.5 * bounds.width();
        const double hh = 0.5 * bounds.computeHeight();
        GcjAffine fit;
        fit.center = gcjDeltaAt(cLng, cLat);
        const GcjDelta east = gcjDeltaAt(cLng + hw, cLat);
        const GcjDelta west = gcjDeltaAt(cLng - hw, cLat);
        const GcjDelta south = gcjDeltaAt(cLng, cLat - hh);
        const GcjDelta north = gcjDeltaAt(cLng, cLat + hh);
        fit.ddu = {east.dLng - west.dLng, east.dLat - west.dLat};
        fit.ddv = {south.dLng - north.dLng, south.dLat - north.dLat};
        return fit;
    }

    GcjDelta at(double u, double v) const {
        return {center.dLng + ddu.dLng * (u - 0.5) + ddv.dLng * (v - 0.5),
                center.dLat + ddu.dLat * (u - 0.5) + ddv.dLat * (v - 0.5)};
    }
};

// 稠密网格上求「精确 GCJ」与「仿射 GCJ」的最大 UV 偏差,折算像素
// (瓦片≈屏幕 256px;两者同用保守 rect1 归一化,故差值只含 δ 近似误差)。
double maxAffineErrorPx(const Rectangle& bounds) {
    const Rectangle rect1 = projectWorldRectangleForRasterOverlay(
        bounds, RasterOverlayProjection::Gcj02WebMercator);
    const GcjAffine fit = GcjAffine::fit(bounds);
    const int kSamples = 33;
    double maxUv = 0.0;
    for (int y = 0; y < kSamples; ++y) {
        const double v = static_cast<double>(y) / (kSamples - 1);
        const double lat = bounds.north() - v * bounds.computeHeight();
        for (int x = 0; x < kSamples; ++x) {
            const double u = static_cast<double>(x) / (kSamples - 1);
            const double lng = bounds.west() + u * bounds.width();
            const Cartographic pos = Cartographic::fromRadians(lng, lat);
            const Vec3 exact = projectWorldPositionForRasterOverlay(
                pos, RasterOverlayProjection::Gcj02WebMercator);
            const GcjDelta delta = fit.at(u, v);
            const Vec3 affine = projectWorldPositionForRasterOverlay(
                Cartographic::fromRadians(lng + delta.dLng,
                                          lat + delta.dLat),
                RasterOverlayProjection::WebMercator);
            maxUv = std::max(
                maxUv,
                std::max(
                    std::abs(exact.x() - affine.x()) / rect1.width(),
                    std::abs(exact.y() - affine.y()) /
                        rect1.computeHeight()));
        }
    }
    return maxUv * 256.0;
}

}  // namespace

// ---- R1:约定复现(标准 WebMercator) ----
TEST(ComputedImageryUvRisk, VsFormulaReproducesBakedStandardMercator) {
    const Rectangle bounds =
        Rectangle::fromDegrees(106.50, 29.55, 106.60, 29.65);
    auto model = EllipsoidTerrainMeshBuilder::makeModel(
        bounds,
        std::vector<RasterOverlayProjection>{
            RasterOverlayProjection::Geographic,
            RasterOverlayProjection::WebMercator},
        8,
        {},
        false,
        false,
        /*buildSkirt=*/true);
    ASSERT_NE(nullptr, model);
    const double err =
        maxSet1Error(*model, RasterOverlayProjection::WebMercator);
    std::printf("[R1] 标准链 maxErr=%.3g(unorm16 档=%.3g)\n", err,
                kUnorm16Step);
    // 容差压在 unorm16 量化档之下:参考实现的重建误差必须低于渲染端
    // 既有的精度地板,否则「算」相对「存」是净退化。
    EXPECT_LT(err, kUnorm16Step * 0.5)
        << "标准链重建误差超过 unorm16 量化档的一半";
}

// ---- R1:约定复现(GCJ,境内瓦片) ----
TEST(ComputedImageryUvRisk, VsFormulaReproducesBakedGcj) {
    const Rectangle bounds =
        Rectangle::fromDegrees(106.50, 29.55, 106.60, 29.65);
    auto model = EllipsoidTerrainMeshBuilder::makeModel(
        bounds,
        std::vector<RasterOverlayProjection>{
            RasterOverlayProjection::Geographic,
            RasterOverlayProjection::Gcj02WebMercator},
        8,
        {},
        false,
        false,
        /*buildSkirt=*/true);
    ASSERT_NE(nullptr, model);
    const double err =
        maxSet1Error(*model, RasterOverlayProjection::Gcj02WebMercator);
    std::printf("[R1] GCJ 链 maxErr=%.3g(unorm16 档=%.3g)\n", err,
                kUnorm16Step);
    EXPECT_LT(err, kUnorm16Step * 0.5)
        << "GCJ 链重建误差超过 unorm16 量化档的一半";
}

// ---- R1:约定复现(GCJ,z10 尺度大瓦片 —— 归一化矩形是保守盒的场景) ----
TEST(ComputedImageryUvRisk, VsFormulaReproducesBakedGcjLargeTile) {
    const Rectangle bounds =
        Rectangle::fromDegrees(106.171875, 29.53522956, 106.5234375,
                               29.84064389);
    auto model = EllipsoidTerrainMeshBuilder::makeModel(
        bounds,
        std::vector<RasterOverlayProjection>{
            RasterOverlayProjection::Geographic,
            RasterOverlayProjection::Gcj02WebMercator},
        16,
        {},
        false,
        false,
        /*buildSkirt=*/true);
    ASSERT_NE(nullptr, model);
    const double err =
        maxSet1Error(*model, RasterOverlayProjection::Gcj02WebMercator);
    std::printf("[R1] GCJ 大瓦片 maxErr=%.3g(unorm16 档=%.3g)\n", err,
                kUnorm16Step);
    EXPECT_LT(err, kUnorm16Step * 0.5)
        << "GCJ 大瓦片重建误差超过 unorm16 量化档的一半";
}

// ---- R5:仿射近似,境内高 zoom(z15 尺度) ----
TEST(ComputedImageryUvRisk, GcjAffineSubpixelAtHighZoomInterior) {
    const Rectangle bounds = Rectangle::fromDegrees(
        106.5036, 29.6118, 106.5146, 29.6213);  // ~z15,重庆
    const double px = maxAffineErrorPx(bounds);
    std::printf("[R5] z15 境内仿射误差 = %.4f px\n", px);
    EXPECT_LT(px, 0.1) << "z15 境内仿射误差 " << px << " px";
}

// ---- R5:仿射近似,境内最坏 zoom(瓦片跨度≈δ 场 sin(6πx) 周期 1/3°) ----
TEST(ComputedImageryUvRisk, GcjAffineSubpixelAtWorstZoomInterior) {
    const Rectangle bounds = Rectangle::fromDegrees(
        106.171875, 29.53522956, 106.5234375, 29.84064389);  // z10
    const double px = maxAffineErrorPx(bounds);
    std::printf("[R5] z10 境内仿射误差 = %.4f px\n", px);
    // 预测 ~0.5px(仿射残差两个上界——曲率界与 2×幅度饱和界——的相交区)。
    // 1px 是「体验上不可见」的裁决线,不是拟合值。
    EXPECT_LT(px, 1.0) << "z10 境内仿射误差 " << px << " px";
}

// ---- R5:仿射近似,跨中国框瓦片(阶跃,已知退化,量化其上界) ----
TEST(ComputedImageryUvRisk, GcjAffineBoundaryTileErrorIsBoundedAndReported) {
    const Rectangle bounds = Rectangle::fromDegrees(
        137.671875, 44.84029066, 138.0234375, 45.08903557);  // z10,跨 137.8347°E
    ASSERT_TRUE(Gcj02CoordinateTransform::crossesChinaBounds(bounds))
        << "样例瓦片必须真的跨框,否则本测试测了个寂寞";
    const double px = maxAffineErrorPx(bounds);
    // 阶跃 δ~500m / (z10 瓦片 39km/256px≈152m/px) ≈ 3.3px 量级;仿射的
    // 中心差分梯度跨阶跃取值,可能再放大。12px 是 sanity 上限——超过它
    // 说明拟合发散,不只是「代表不了阶跃」。真实数值打印供体验裁决
    // (境外侧;境内视角不可见)。
    RecordProperty("boundary_affine_error_px", px);
    std::printf("[R5] 跨框瓦片仿射误差 = %.2f px(境外侧可见,境内不可见)\n",
                px);
    EXPECT_LT(px, 12.0) << "跨框瓦片仿射误差发散:" << px << " px";
    // 配套判据:crossesChinaBounds 就是产线上把这类瓦片挡出仿射路径的闸
    //(与 TerrainPageStore 跨界瓦片放弃视锥剔除同一判据)。
}
