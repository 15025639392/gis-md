// P0 精度冒烟（北极星 Phase 2c，地形 GPU 位移重构）。
//
// 目标：证明"把椭球位移从 CPU 烘焙挪进顶点 shader"在 float32 瓦片局部
// (RTC) 空间里保精度——GPU 路径的 per-vertex 位置与现有 CPU 烘焙路径
// (cartographicToCartesian(lat,lng,h) 双精度算完再降 f32) 逐顶点 < 1m。
//
// 数学地基（见 docs/issues/terrain-gpu-displacement-redesign-2026-07-20.md §5）：
//   cartographicToCartesian(lat,lng,h)
//     ≡ cartographicToCartesian(lat,lng,0) + geodeticSurfaceNormal(lat,lng)·h
// 因 geodeticSurfaceNormal 只依赖 lat/lng、height 项在参考实现里是线性叠加
// (Ellipsoid.cpp:46-54,81-88)。所以 GPU 位移 pos = 面点_local + 法线_local·elev
// 与现 CPU 路径**代数等价**，唯一新增误差 = 单位法线·elev 在 f32 里的舍入。
//
// 本测试是纯 host 单测（无设备/shader）：
//   - 参考(truth)   = 双精度 (cartographicToCartesian(lat,lng,h) - originEcef)
//   - 现有路径(A)   = f32(参考)                         —— 现在 VBO 里存的
//   - GPU 位移(B)   = f32(面点_local) + f32(法线)·f32(h) —— shader 将算的
// 断言（精度模型见下）：
//   ① B vs 参考 < 1m               —— 绝对正确性闸（P0 spec）
//   ② B vs 参考 ≲ A vs 参考（同阶）—— 无回归：GPU 位移不比现 shipping f32 RTC 更险
//   ③ z12 native LOD 下 B vs 参考 < 1cm —— 真实地形 LOD 富余
//
// 精度模型（实测坐实，别再拿"B≈A 逐值相等"当断言）：粗 LOD 下瓦片局部坐标
// 量级大（z3=45° 跨度→局部 ~2500km），f32 ULP ~0.30m。A 把"位移后"的局部降
// f32、B 把"面点"局部降 f32 再 f32 加 normal·h——**两次舍入独立**，各自对 truth
// ~0.3m 误差，但彼此可差 ~0.3m。所以正确的 de-risk 陈述是"B 对 truth 与 A 对
// truth **同阶**"（②），不是"B≈A"。二者都 < 1m，且随 LOD 变细急剧收敛（z12 ~mm）。

#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/math/Vec3.h"

using namespace earth_engine;

namespace {

// 模拟 GPU 顶点 shader 端的 float32 位移：全部以 float 运算。
// 对应 kTerrainVertexGLSL/MSL 未来将做的 morphPos = surfaceLocal + up*elev。
struct Vec3f {
    float x, y, z;
};

Vec3f toF32(const Vec3& v) {
    return {static_cast<float>(v.x()), static_cast<float>(v.y()),
            static_cast<float>(v.z())};
}

// shader 端位移：面点局部坐标(f32) + 单位法线(f32) * 高程(f32)。
Vec3f displaceF32(const Vec3f& surfaceLocal, const Vec3f& normal, float elev) {
    return {surfaceLocal.x + normal.x * elev, surfaceLocal.y + normal.y * elev,
            surfaceLocal.z + normal.z * elev};
}

// 双精度参考位置的 f32 下采（= 现有 CPU 烘焙路径存进 VBO 的值）。
double distance(const Vec3f& a, const Vec3& bDouble) {
    const double dx = static_cast<double>(a.x) - bDouble.x();
    const double dy = static_cast<double>(a.y) - bDouble.y();
    const double dz = static_cast<double>(a.z) - bDouble.z();
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double distance(const Vec3f& a, const Vec3f& b) {
    const double dx = static_cast<double>(a.x) - static_cast<double>(b.x);
    const double dy = static_cast<double>(a.y) - static_cast<double>(b.y);
    const double dz = static_cast<double>(a.z) - static_cast<double>(b.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// 一块以 (centerLat,centerLng) 为中心、跨 spanRad 的方形瓦片，65×65 栅格。
// gridN=65 = 2^6+1（GE 嵌套栅格约定）。逐顶点跑两条路径并取最大误差。
struct TileError {
    double maxCandidateVsTruth = 0.0;   // B(GPU f32) vs 双精度参考
    double maxCandidateVsCurrent = 0.0;  // B(GPU f32) vs A(现有路径 f32)
    double maxCurrentVsTruth = 0.0;      // A(现有 f32) vs 双精度参考（基线对照）
};

TileError evaluateTile(double centerLatRad, double centerLngRad,
                       double spanRad, double elevation, int gridN = 65) {
    const Ellipsoid& e = Ellipsoid::WGS84();

    // RTC 原点 = 瓦片中心的椭球面点（与 makeModel 的 preferredLocalOriginEcef 同）。
    const Vec3 originEcef =
        e.cartographicToCartesian(Cartographic::fromRadians(
            centerLngRad, centerLatRad, 0.0));

    const double half = spanRad * 0.5;
    TileError out;

    for (int j = 0; j < gridN; ++j) {
        const double v = static_cast<double>(j) / (gridN - 1);   // 0..1
        const double lat = centerLatRad - half + v * spanRad;
        for (int i = 0; i < gridN; ++i) {
            const double u = static_cast<double>(i) / (gridN - 1);
            const double lng = centerLngRad - half + u * spanRad;

            const Cartographic c = Cartographic::fromRadians(lng, lat, 0.0);

            // 双精度参考（truth）：完整 cartographicToCartesian(带高程) - 原点。
            const Vec3 refEcef = e.cartographicToCartesian(
                Cartographic::fromRadians(lng, lat, elevation));
            const Vec3 refLocal = refEcef - originEcef;

            // 现有路径(A)：直接把参考局部坐标降 f32（VBO 里现在存的）。
            const Vec3f current = toF32(refLocal);

            // GPU 位移(B)：面点局部(f32) + 法线(f32) * elev(f32)。
            const Vec3 surfaceLocal =
                e.cartographicToCartesian(c) - originEcef;  // 双精度算面点局部
            const Vec3f surfaceLocalF = toF32(surfaceLocal);  // 模板 VBO 存 f32
            const Vec3f normalF = toF32(e.geodeticSurfaceNormal(c));
            const Vec3f candidate =
                displaceF32(surfaceLocalF, normalF, static_cast<float>(elevation));

            out.maxCandidateVsTruth =
                std::max(out.maxCandidateVsTruth, distance(candidate, refLocal));
            out.maxCandidateVsCurrent =
                std::max(out.maxCandidateVsCurrent, distance(candidate, current));
            out.maxCurrentVsTruth =
                std::max(out.maxCurrentVsTruth, distance(current, refLocal));
        }
    }
    return out;
}

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

// 绝对正确性闸：GPU 位移 vs 双精度参考 < 1m。
constexpr double kAbsoluteBudgetMeters = 1.0;
// 无回归：B 对 truth 不显著劣于 A 对 truth。两者是相近量级的独立 f32 舍入，
// 最坏 ~2-3× 关系 + 亚毫米 normal·h 噪声——4× + 5mm 是稳健上界。
constexpr double kParityFactor = 4.0;
constexpr double kParitySlackMeters = 0.005;

}  // namespace

// ============================================================
// 核心断言：真实地形 LOD 范围内，GPU 位移逐顶点 < 1m 且几乎不比现路径差。
// ============================================================

// 覆盖 LOD 粗→细（span 45°→~0.09°）、纬度 0/45/72°、高程 0/9000m 的组合。
// span 用 360°/2^lod 近似瓦片经度跨度，lod 从 3（很粗，最坏 RTC 量级）起。
TEST(TerrainGpuDisplacementPrecision, PerVertexWithinBudgetAcrossLodLatElev) {
    struct Case {
        int lod;
        double latDeg;
        double elevM;
    };
    const Case cases[] = {
        {3, 0.0, 0.0},     {3, 45.0, 9000.0}, {3, 72.0, 9000.0},
        {5, 0.0, 9000.0},  {5, 60.0, 9000.0}, {8, 45.0, 9000.0},
        {10, 72.0, 9000.0}, {12, 30.0, 9000.0}, {12, 80.0, 9000.0},
    };

    for (const Case& c : cases) {
        const double spanRad = (360.0 / std::pow(2.0, c.lod)) * kDegToRad;
        const TileError err =
            evaluateTile(c.latDeg * kDegToRad, /*centerLng=*/116.0 * kDegToRad,
                         spanRad, c.elevM);

        const std::string label = "lod=" + std::to_string(c.lod) +
                                  " lat=" + std::to_string(c.latDeg) +
                                  " elev=" + std::to_string(c.elevM);

        // ① 绝对正确性：GPU 位移 vs 双精度真值 < 1m。
        EXPECT_LT(err.maxCandidateVsTruth, kAbsoluteBudgetMeters)
            << "candidate-vs-truth exceeded 1m at " << label;

        // ② 无回归：B 对 truth 不显著劣于 A(现 shipping f32 RTC) 对 truth。
        EXPECT_LT(err.maxCandidateVsTruth,
                  kParityFactor * err.maxCurrentVsTruth + kParitySlackMeters)
            << "GPU displacement error materially exceeds shipping f32 RTC at "
            << label << " (candidate=" << err.maxCandidateVsTruth
            << "m current=" << err.maxCurrentVsTruth << "m)";

        // 可见性：把每个 case 的实测精度记进测试属性（CI 里可查）。
        RecordProperty(label + ":candidateVsTruth_m",
                       std::to_string(err.maxCandidateVsTruth));
        RecordProperty(label + ":currentVsTruth_m",
                       std::to_string(err.maxCurrentVsTruth));
    }
}

// 显式钉住"最坏 RTC 量级"：最粗受测 LOD(3) + 高纬 + 满高程，
// 现有 f32 路径本身的误差也应 < 1m（证明 GPU 路径不比现状更险）。
TEST(TerrainGpuDisplacementPrecision, WorstCaseCoarseTileCurrentPathAlsoWithin1m) {
    const double spanRad = (360.0 / std::pow(2.0, 3.0)) * kDegToRad;  // 45°
    const TileError err = evaluateTile(72.0 * kDegToRad, 116.0 * kDegToRad,
                                       spanRad, 9000.0);

    // 现有 shipping 路径在此最坏量级的 f32 RTC 误差（信息性——GPU 路径应≈此）。
    EXPECT_LT(err.maxCurrentVsTruth, kAbsoluteBudgetMeters)
        << "shipping f32 RTC path itself exceeds 1m at coarse tile (current="
        << err.maxCurrentVsTruth << "m)";
    // GPU 位移 vs truth < 1m，且不显著劣于现路径。
    EXPECT_LT(err.maxCandidateVsTruth, kAbsoluteBudgetMeters);
    EXPECT_LT(err.maxCandidateVsTruth,
              kParityFactor * err.maxCurrentVsTruth + kParitySlackMeters);
    RecordProperty("coarseWorstCase:candidateVsTruth_m",
                   std::to_string(err.maxCandidateVsTruth));
    RecordProperty("coarseWorstCase:currentVsTruth_m",
                   std::to_string(err.maxCurrentVsTruth));
}

// 生产真实地形 LOD(z12 native cap) 应远优于闸门——亚厘米。
TEST(TerrainGpuDisplacementPrecision, NativeTerrainLodIsSubCentimeter) {
    const double spanRad = (360.0 / std::pow(2.0, 12.0)) * kDegToRad;  // ~0.088°
    const TileError err = evaluateTile(29.617 * kDegToRad,   // 重庆 grazing preset
                                       106.508 * kDegToRad, spanRad, 9000.0);
    EXPECT_LT(err.maxCandidateVsTruth, 0.01)
        << "z12 candidate-vs-truth should be sub-cm (got "
        << err.maxCandidateVsTruth << "m)";
    RecordProperty("z12:candidateVsTruth_m",
                   std::to_string(err.maxCandidateVsTruth));
}
