#include <gtest/gtest.h>

#include "earth_engine/content/EllipsoidTerrainMeshBuilder.h"
#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Gcj02CoordinateTransform.h"
#include "earth_engine/core/geodesy/WebMercatorProjection.h"
#include "earth_engine/core/math/MathUtils.h"
#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/tiling/RasterOverlayProjection.h"
#include "earth_engine/tiling/TileSurface.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

using namespace earth_engine;

// =============================================================================
// I-P6 量化:fill 代理 UV 仍用地形 scheme 投影(GCJ-02 下二阶误差,米级)。
//
// 生产链(2026-08-22 静态核实):
//   - fill 代理 tile 无 committed render content → 映射走 bounding-volume 回退
//     (TileRasterOverlayMappingPolicy::contextFor hasRenderContentDetails=false);
//   - fill 代理模型只有地形投影 texcoord set0:EllipsoidTerrainMeshBuilder::
//     makeModel 用 TerrainRasterOverlayProjectionResolver::forTileKey 的 projection
//     (OpenGlobus-Earth = plain WebMercator)生成 set0,归一化于 plain-merc 矩形 r;
//   - pageStore 高清路径明确跳过 fill(applyToTerrainCommand 非 RealTerrain return),
//     影像留在 directComposite:overlayUv = tileUV.xy + uvFromSet(set0)*tileUV.zw;
//   - tileUV offset/scale = computeTranslationAndScale(R_G, T),R_G = GCJ-merc
//     矩形(投影包围盒),T = 影像源瓦矩形(同 zoom 时 ≈ R_G)。
//
// 结论推导:采样到的源位置 S = T.west + (offset + uv0*scale)*T.width
//   = R_G.west + uv0*R_G.width(T 抵消),即把 plain-merc UV 张满 GCJ 矩形;
//   正确位置 G = merc(GCJ(wgs(v)))。常量 δ 平移被映射吸收,残差 = δ(west)-δ(v),
//   = δ 场在瓦片内的梯度×跨距(二阶误差)。本测试用生产函数逐顶点复现这条
//   采样链与精确 GCJ 对拍,输出米/像素误差,供「是否立项修复」裁决。
// =============================================================================

namespace {

constexpr double kTileScreenPixels = 256.0;

// 真实 WebMercator 瓦片的经纬包围盒(OpenGlobus-Earth 地形 scheme 同源)。
Rectangle webMercatorTileBounds(int zoom, int x, int y) {
    const WebMercatorProjection proj(Ellipsoid::WGS84());
    const double tiles = static_cast<double>(1u << zoom);
    const double size = MathUtils::TwoPi / tiles;
    // unproject 的输入是米制(x = lon_rad * R),不能用弧度直接喂。
    const double westMerc = static_cast<double>(x) * size - MathUtils::OnePi;
    const double eastMerc =
        static_cast<double>(x + 1) * size - MathUtils::OnePi;
    const double northMerc = MathUtils::OnePi -
                             static_cast<double>(y) * size;
    const double southMerc = MathUtils::OnePi -
                             static_cast<double>(y + 1) * size;
    const double r = proj.semimajorAxis();
    const Cartographic nw =
        proj.unproject(Vec3(westMerc * r, northMerc * r, 0.0));
    const Cartographic se =
        proj.unproject(Vec3(eastMerc * r, southMerc * r, 0.0));
    return Rectangle(nw.longitude(), se.latitude(),
                     se.longitude(), nw.latitude());
}

// 某经纬度所在的 WebMercator 瓦片 x/y(与 OpenGlobus-Earth 行列约定一致:
// y 从北向南增长)。
std::pair<int, int> webMercatorTileXy(double lngRad, double latRad, int zoom) {
    const double tiles = static_cast<double>(1u << zoom);
    const int x = static_cast<int>(std::floor(
        (lngRad + MathUtils::OnePi) / MathUtils::TwoPi * tiles));
    const double sinLat = std::sin(latRad);
    const double mercY = 0.5 * std::log(
        (1.0 + sinLat) / (1.0 - sinLat));
    const int y = static_cast<int>(std::floor(
        (1.0 - mercY / MathUtils::OnePi) * 0.5 * tiles));
    return {x, y};
}

// 复现 fill 代理 directComposite 采样链,逐顶点量化采样位置 vs 精确 GCJ 位置。
// @return maxErrorMeters / maxErrorPx(瓦片≈256px)/ maxErrorFraction(占瓦片比)。
struct FillProxyError {
    double maxMeters = 0.0;
    double maxPx = 0.0;
    double maxFraction = 0.0;
};

FillProxyError quantifyFillProxyGcjError(const Rectangle& bounds,
                                         int gridSize) {
    const RasterOverlayProjection terrainProjection =
        RasterOverlayProjection::WebMercator;  // OpenGlobus-Earth 地形 scheme
    const RasterOverlayProjection sourceProjection =
        RasterOverlayProjection::Gcj02WebMercator;

    // fill 代理模型:生产 makeModel 的 set0 = plain merc UV。
    std::unique_ptr<GltfModel> model = EllipsoidTerrainMeshBuilder::makeModel(
        bounds,
        std::vector<RasterOverlayProjection>{terrainProjection},
        gridSize,
        {},
        /*computeGridNormals=*/false,
        /*computeGeomorphDelta=*/false,
        /*buildSkirt=*/false);
    EXPECT_NE(nullptr, model);
    if (!model || model->primitives.empty()) {
        return {};
    }
    const GltfPrimitive& primitive = model->primitives.front();
    if (primitive.vertexTexCoords[0].size() != primitive.vertices.size()) {
        ADD_FAILURE() << "fill 模型 set0 texcoord 数与顶点数不一致";
        return {};
    }

    // 映射矩形:GCJ 投影包围盒(生产 bounding-volume 回退路径)。
    const Rectangle rG = projectWorldRectangleForRasterOverlay(
        bounds, sourceProjection);
    // 影像源瓦矩形:同 zoom 1:1 贴合的常见态(祖先回退只加模糊不加位移,
    // 且映射 offset/scale 换算后采样位置与 T 无关,推导见文件头)。
    const Rectangle t = rG;
    const TileTextureWindow native =
        TileSurface::computeTranslationAndScale(rG, t);
    const TileTextureWindow nw =
        TileSurface::textureWindowForNorthWestUv(native);

    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    FillProxyError error;
    double worstMetersU = 0.0;
    double worstMetersV = 0.0;
    for (size_t i = 0; i < primitive.vertices.size(); ++i) {
        const std::optional<Cartographic> cartographic =
            ellipsoid.tryCartesianToCartographic(
                primitive.vertices[i].positionEcef);
        if (!cartographic) {
            continue;
        }
        const float u0 = primitive.vertexTexCoords[0][i][0];
        const float v0 = primitive.vertexTexCoords[0][i][1];

        // 生产采样位置(TileSurface NW 换算,等价于 shader
        // overlayUv = tileUV.xy + uvFromSet*scale)。
        const double sampledU =
            t.west() + (static_cast<double>(nw.offsetU) +
                        static_cast<double>(u0) * nw.scaleU) *
                           t.width();
        const double sampledV =
            t.north() - (static_cast<double>(nw.offsetV) +
                         static_cast<double>(v0) * nw.scaleV) *
                            t.height();
        // 闭合形式核对(推导:S = R_G.west + u0*R_G.width /
        // R_G.north - v0*R_G.height),证明换算链本身没错位。
        EXPECT_NEAR(sampledU, rG.west() + static_cast<double>(u0) * rG.width(),
                    1e-6 * std::max(1.0, std::abs(rG.width())));
        EXPECT_NEAR(sampledV,
                    rG.north() - static_cast<double>(v0) * rG.height(),
                    1e-6 * std::max(1.0, std::abs(rG.height())));

        // 精确 GCJ 采样位置。
        const Vec3 exact = projectWorldPositionForRasterOverlay(
            *cartographic, sourceProjection);
        const double errU = sampledU - exact.x();
        const double errV = sampledV - exact.y();
        worstMetersU = std::max(worstMetersU, std::abs(errU));
        worstMetersV = std::max(worstMetersV, std::abs(errV));
    }
    const double metersPerPxU = rG.width() / kTileScreenPixels;
    const double metersPerPxV = rG.height() / kTileScreenPixels;
    error.maxMeters = std::max(worstMetersU, worstMetersV);
    error.maxPx = std::max(worstMetersU / metersPerPxU,
                           worstMetersV / metersPerPxV);
    error.maxFraction = std::max(
        worstMetersU / std::max(rG.width(), 1e-12),
        worstMetersV / std::max(rG.height(), 1e-12));
    return error;
}

}  // namespace

// ---- 境内 zoom 阶梯:fill 代理 GCJ 采样误差(H1 vs H2 的裁决证据) ----
TEST(FillProxyGcjUvRisk, InteriorZoomLadderChongqing) {
    constexpr double kLngDeg = 106.508;
    constexpr double kLatDeg = 29.617;
    const double lng = kLngDeg * MathUtils::OnePi / 180.0;
    const double lat = kLatDeg * MathUtils::OnePi / 180.0;

    double worstMeters = 0.0;
    double worstPx = 0.0;
    int worstZoomM = -1;
    int worstZoomPx = -1;
    std::printf("zoom  maxMeters  maxPx(256px瓦)  maxFraction\n");
    for (int zoom = 6; zoom <= 18; ++zoom) {
        const auto [x, y] = webMercatorTileXy(lng, lat, zoom);
        const Rectangle bounds = webMercatorTileBounds(zoom, x, y);
        ASSERT_FALSE(Gcj02CoordinateTransform::crossesChinaBounds(bounds))
            << "z" << zoom << " 瓦片跨中国框,不属于境内阶梯";
        const FillProxyError error =
            quantifyFillProxyGcjError(bounds, /*gridSize=*/16);
        std::printf("z%-2d  %9.3f m  %9.4f px  %9.6f\n",
                    zoom, error.maxMeters, error.maxPx, error.maxFraction);
        if (error.maxMeters > worstMeters) {
            worstMeters = error.maxMeters;
            worstZoomM = zoom;
        }
        if (error.maxPx > worstPx) {
            worstPx = error.maxPx;
            worstZoomPx = zoom;
        }
        // 量化结论:境内亚像素(256px 瓦片)。钉死 < 1px,防止 GCJ 常数或
        // 采样链被改动后静默退化到像素级。
        EXPECT_LT(error.maxPx, 1.0) << "z" << zoom << " fill 代理误差上 1px";
    }
    RecordProperty("worst_meters", worstMeters);
    RecordProperty("worst_meters_zoom", worstZoomM);
    RecordProperty("worst_px", worstPx);
    RecordProperty("worst_px_zoom", worstZoomPx);
    std::printf("[I-P6] 重庆境内最坏:%.3f m @ z%d;%.4f px @ z%d\n",
                worstMeters, worstZoomM, worstPx, worstZoomPx);
}

// ---- 全国扫描:找 fill 代理 GCJ 误差的境内全局最坏位置 ----
TEST(FillProxyGcjUvRisk, ChinaWideInteriorWorstCase) {
    const double lngDegs[] = {73.5,  75.0,  76.5,  78.0,  79.5,  81.0,
                              82.5,  84.0,  85.5,  87.0,  88.5,  90.0,
                              91.5,  93.0,  94.5,  96.0,  97.5,  99.0,
                              100.5, 102.0, 103.5, 105.0, 106.5, 108.0,
                              109.5, 111.0, 112.5, 114.0, 115.5, 117.0,
                              118.5, 120.0, 121.5, 123.0, 124.5, 126.0,
                              127.5, 129.0, 130.5, 132.0, 133.5, 135.0,
                              136.5};
    const double latDegs[] = {21.0, 25.0, 30.0, 35.0, 40.0, 45.0, 49.0};

    double worstPx = 0.0;
    double worstMeters = 0.0;
    double worstLng = 0.0;
    double worstLat = 0.0;
    int worstZoomPx = -1;
    int worstZoomM = -1;
    int interiorTiles = 0;
    for (double lngDeg : lngDegs) {
        for (double latDeg : latDegs) {
            const double lng = lngDeg * MathUtils::OnePi / 180.0;
            const double lat = latDeg * MathUtils::OnePi / 180.0;
            for (int zoom = 6; zoom <= 18; ++zoom) {
                const auto [x, y] = webMercatorTileXy(lng, lat, zoom);
                const Rectangle bounds = webMercatorTileBounds(zoom, x, y);
                if (Gcj02CoordinateTransform::crossesChinaBounds(bounds)) {
                    continue;  // 跨框瓦片单独量化(见 Boundary 测试)
                }
                ++interiorTiles;
                const FillProxyError error =
                    quantifyFillProxyGcjError(bounds, /*gridSize=*/8);
                if (error.maxPx > worstPx) {
                    worstPx = error.maxPx;
                    worstLng = lngDeg;
                    worstLat = latDeg;
                    worstZoomPx = zoom;
                }
                if (error.maxMeters > worstMeters) {
                    worstMeters = error.maxMeters;
                    worstZoomM = zoom;
                }
            }
        }
    }
    RecordProperty("china_wide_interior_tiles", interiorTiles);
    RecordProperty("china_wide_worst_px", worstPx);
    RecordProperty("china_wide_worst_px_zoom", worstZoomPx);
    RecordProperty("china_wide_worst_meters", worstMeters);
    RecordProperty("china_wide_worst_meters_zoom", worstZoomM);
    std::printf("[I-P6] 全国境内 %d 瓦片最坏:%.4f px @ (%g,%g) z%d;%.3f m @ z%d\n",
                interiorTiles, worstPx, worstLng, worstLat, worstZoomPx,
                worstMeters, worstZoomM);
    EXPECT_GT(interiorTiles, 1000) << "全国扫描样本过少,结论不成立";
    // 量化结论:境内最坏 ~1.2px(高纬 z17),粗扫只用于找位置 + sanity。
    EXPECT_LT(worstPx, 2.0) << "境内最坏 fill 代理误差发散";
}

// ---- 高纬细扫:全国粗扫峰值(105,49)z17 一带,gridSize 16 精算真实上界 ----
TEST(FillProxyGcjUvRisk, HighLatitudeRefinedWorstCase) {
    double worstPx = 0.0;
    double worstLng = 0.0;
    double worstLat = 0.0;
    int worstZoom = -1;
    for (double lngDeg = 100.0; lngDeg <= 115.0; lngDeg += 0.75) {
        for (double latDeg = 45.0; latDeg <= 53.0; latDeg += 1.0) {
            const double lng = lngDeg * MathUtils::OnePi / 180.0;
            const double lat = latDeg * MathUtils::OnePi / 180.0;
            for (int zoom = 10; zoom <= 18; ++zoom) {
                const auto [x, y] = webMercatorTileXy(lng, lat, zoom);
                const Rectangle bounds = webMercatorTileBounds(zoom, x, y);
                if (Gcj02CoordinateTransform::crossesChinaBounds(bounds)) {
                    continue;
                }
                const FillProxyError error =
                    quantifyFillProxyGcjError(bounds, /*gridSize=*/16);
                if (error.maxPx > worstPx) {
                    worstPx = error.maxPx;
                    worstLng = lngDeg;
                    worstLat = latDeg;
                    worstZoom = zoom;
                }
            }
        }
    }
    RecordProperty("high_lat_worst_px", worstPx);
    RecordProperty("high_lat_worst_px_pos", worstLng);
    RecordProperty("high_lat_worst_px_lat", worstLat);
    RecordProperty("high_lat_worst_px_zoom", worstZoom);
    std::printf("[I-P6] 高纬细扫最坏:%.4f px @ (%g,%g) z%d\n",
                worstPx, worstLng, worstLat, worstZoom);
    EXPECT_LT(worstPx, 2.0) << "高纬 fill 代理误差发散";
}

// ---- 跨中国框瓦片:阶跃 δ 已知退化,量化上界供体验裁决 ----
TEST(FillProxyGcjUvRisk, BoundaryTileErrorIsBoundedAndReported) {
    const Rectangle bounds = Rectangle::fromDegrees(
        137.671875, 44.84029066, 138.0234375, 45.08903557);  // z10,跨 137.8347°E
    ASSERT_TRUE(Gcj02CoordinateTransform::crossesChinaBounds(bounds))
        << "样例瓦片必须真的跨框,否则本测试测了个寂寞";
    const FillProxyError error =
        quantifyFillProxyGcjError(bounds, /*gridSize=*/16);
    RecordProperty("boundary_error_meters", error.maxMeters);
    RecordProperty("boundary_error_px", error.maxPx);
    RecordProperty("boundary_error_fraction", error.maxFraction);
    std::printf("[I-P6] 跨框瓦片 fill 代理误差 = %.2f m / %.2f px(占瓦 %.3f)\n",
                error.maxMeters, error.maxPx, error.maxFraction);
    // sanity:阶跃 δ~500m 在 z10 瓦片(39km/256px≈152m/px)约 3.3px;12px 是
    // 发散阈值,真实数值打印供体验裁决(境外侧;境内视角不可见)。
    EXPECT_LT(error.maxPx, 12.0) << "跨框 fill 代理误差发散";
}
