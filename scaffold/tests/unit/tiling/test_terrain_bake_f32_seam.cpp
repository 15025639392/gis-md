// B 方案(GPU f32 烘焙)无缝精度守卫。
//
// GPU 片元着色器(TerrainHeightBakeShader.h)无法在 host 无 GL 上下文里跑,故这里
// 忠实复刻 bakeTerrainHeightNormalTexels 的数学(514 cell-registered+0.5px inset
// 重叠环 → (n+2)² scratch 双线性重采样 → 三点不等臂中心差分 → 8bit 法线编码),
// 用模板 <T> 参数化精度:double 和 float 走同一份代码,唯一差别是精度。
//
// 锁两条不变量(任一破 → 移 GPU 前必须停):
//   ① 同瓦片 double vs f32 逐 texel 法线字节差 ≤ 1 LSB(移 GPU 无可见变化)
//   ② f32 相邻瓦片共享边法线夹角 < 2.0°(无缝北极星守住,与 CPU 版同阈值)
//
// 与 test_terrain_edge_normal_seam.cpp 同款解析场(高频项+相位偏移防自掩盖),
// 见那份文件对这两处刻意设计的详解。
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthR = 6378137.0;

struct HeightField {
    double kx, ky;
    double operator()(double lon, double lat) const {
        const double hx = kx * 64.0, hy = ky * 64.0;
        return 1200.0 * std::sin(kx * lon) * std::cos(ky * lat) +
               400.0 * std::sin(3.0 * kx * lon) +
               300.0 * std::cos(2.0 * ky * lat) +
               300.0 * std::sin(hx * lon + 0.7853981634) *
                   std::cos(hy * lat + 0.3926990817);
    }
};

struct Rect {
    double west, south, east, north;
    double width() const { return east - west; }
    double height() const { return north - south; }
};

// 514 cell-registered + 0.5px inset 源(生产 Terrain-RGB 形状),像素直接取解析场
// → 重叠环(px 0/513)天然携带真实邻瓦值。
struct Source {
    static constexpr int kSize = 514;
    static constexpr double kInset = 0.5;
    std::vector<float> h;
    Source(const Rect& b, const HeightField& f) : h(kSize * kSize) {
        const double span = (kSize - 1) - 2.0 * kInset;
        for (int py = 0; py < kSize; ++py) {
            const double v = (py - kInset) / span;
            const double lat = b.north - v * b.height();
            for (int px = 0; px < kSize; ++px) {
                const double u = (px - kInset) / span;
                const double lon = b.west + u * b.width();
                h[py * kSize + px] = static_cast<float>(f(lon, lat));
            }
        }
    }
    // 归一化 (u,v)∈[-reach,1+reach] → 源像素双线性(unclamped)。以 T 精度算权重。
    template <typename T>
    T sample(T u, T v) const {
        const double span = (kSize - 1) - 2.0 * kInset;
        const T fx = static_cast<T>(u * span + kInset);
        const T fy = static_cast<T>(v * span + kInset);
        const int x0 = static_cast<int>(std::floor(fx));
        const int y0 = static_cast<int>(std::floor(fy));
        const T tx = fx - static_cast<T>(x0);
        const T ty = fy - static_cast<T>(y0);
        auto at = [&](int x, int y) -> T {
            x = std::clamp(x, 0, kSize - 1);
            y = std::clamp(y, 0, kSize - 1);
            return static_cast<T>(h[static_cast<size_t>(y) * kSize + x]);
        };
        const T a = at(x0, y0), bb = at(x0 + 1, y0);
        const T c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
        return (a * (1 - tx) + bb * tx) * (1 - ty) +
               (c * (1 - tx) + d * tx) * ty;
    }
};

// 三点不等臂一阶导(与生产逐字一致,以 T 精度)。
template <typename T>
T deriv3(T fL, T fC, T fR, T dL, T dR) {
    if (dL <= 0) return dR > 0 ? (fR - fC) / dR : T(0);
    if (dR <= 0) return (fC - fL) / dL;
    return (-dR * dR * fL + (dR * dR - dL * dL) * fC + dL * dL * fR) /
           (dL * dR * (dL + dR));
}

// 以 T 精度烘焙 → 8bit RGBA texel(只关心 B/A 法线)。
template <typename T>
std::vector<uint8_t> bake(const Source& src, const Rect& b, int gridSize) {
    const int n = gridSize + 1;
    const int ns = n + 2;
    // overscanReach = borderInset/span = 0.5/512(真实值,见 TerrainProvider.h)。
    const T reach = static_cast<T>(Source::kInset /
                                   ((Source::kSize - 1) - 2.0 * Source::kInset));
    std::vector<T> axis(ns);
    axis[0] = -reach;
    for (int k = 1; k <= n; ++k)
        axis[k] = static_cast<T>(k - 1) / static_cast<T>(gridSize);
    axis[ns - 1] = static_cast<T>(1) + reach;

    std::vector<T> node(static_cast<size_t>(ns) * ns);
    for (int j = 0; j < ns; ++j)
        for (int i = 0; i < ns; ++i)
            node[static_cast<size_t>(j) * ns + i] = src.sample<T>(axis[i], axis[j]);

    const T centerLat = static_cast<T>(0.5 * (b.north + b.south));
    const T widthM = std::max(
        T(1), static_cast<T>(b.width()) * std::cos(centerLat) * static_cast<T>(kEarthR));
    const T heightM =
        std::max(T(1), static_cast<T>(b.height()) * static_cast<T>(kEarthR));

    std::vector<uint8_t> out(static_cast<size_t>(n) * n * 4, 0);
    auto enc = [](T c) {
        return static_cast<uint8_t>(
            std::lround(std::clamp(c * T(0.5) + T(0.5), T(0), T(1)) * T(255)));
    };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const int si = i + 1, sj = j + 1;
            auto at = [&](int gi, int gj) { return node[static_cast<size_t>(gj) * ns + gi]; };
            const T dLu = (axis[si] - axis[si - 1]) * widthM;
            const T dRu = (axis[si + 1] - axis[si]) * widthM;
            const T dLv = (axis[sj] - axis[sj - 1]) * heightM;
            const T dRv = (axis[sj + 1] - axis[sj]) * heightM;
            const T gU = deriv3<T>(at(si - 1, sj), at(si, sj), at(si + 1, sj), dLu, dRu);
            const T gV = deriv3<T>(at(si, sj - 1), at(si, sj), at(si, sj + 1), dLv, dRv);
            const T inv = T(1) / std::sqrt(gU * gU + gV * gV + T(1));
            const size_t idx = (static_cast<size_t>(j) * n + i) * 4;
            out[idx + 2] = enc(-gU * inv);
            out[idx + 3] = enc(-gV * inv);
        }
    return out;
}

struct Nrm { double x, y, z; };
Nrm decodeNormal(const std::vector<uint8_t>& t, int n, int i, int j) {
    const size_t idx = (static_cast<size_t>(j) * n + i) * 4;
    const double x = t[idx + 2] / 255.0 * 2 - 1, y = t[idx + 3] / 255.0 * 2 - 1;
    const double zz = 1 - x * x - y * y;
    return {x, y, zz > 0 ? std::sqrt(zz) : 0};
}
double angleDeg(const Nrm& a, const Nrm& b) {
    return std::acos(std::clamp(a.x * b.x + a.y * b.y + a.z * b.z, -1.0, 1.0)) * 180 /
           kPi;
}

// z8 x100 y60 与东西邻 x101,Geographic-TMS 几何(经纬跨度 = 180/2^8 度)。
struct NeighborPair {
    Rect a, b;
    HeightField field;
    NeighborPair() {
        const double deg = kPi / 180.0;
        const double tileSpan = 180.0 / 256.0 * deg;
        const double west = -kPi + 100 * tileSpan;
        const double south = -kPi / 2 + 60 * tileSpan;
        a = Rect{west, south, west + tileSpan, south + tileSpan};
        b = Rect{west + tileSpan, south, west + 2 * tileSpan, south + tileSpan};
        field.kx = 2.0 * kPi / (4.0 * a.width());
        field.ky = 2.0 * kPi / (4.0 * a.height());
    }
};

constexpr double kMaxSeamAngleDegrees = 2.0;

}  // namespace

// ① 移 CPU→GPU(double→f32)后同瓦片法线逐 texel 无可见变化(≤1 LSB)。
TEST(TerrainBakeF32SeamTest, Float32MatchesDoubleWithinOneLsb) {
    NeighborPair np;
    Source sA(np.a, np.field);
    for (int gs : {64, 256}) {
        const int n = gs + 1;
        const auto dA = bake<double>(sA, np.a, gs);
        const auto fA = bake<float>(sA, np.a, gs);
        int maxByteDiff = 0;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const size_t idx = (static_cast<size_t>(j) * n + i) * 4;
                maxByteDiff = std::max({maxByteDiff,
                                        std::abs(dA[idx + 2] - fA[idx + 2]),
                                        std::abs(dA[idx + 3] - fA[idx + 3])});
            }
        EXPECT_LE(maxByteDiff, 1)
            << "grid" << gs << ":f32 法线偏离 double " << maxByteDiff
            << " LSB(>1 说明移 GPU 会有可见变化)";
    }
}

// ② f32 烘焙的相邻瓦片共享边法线仍守 2.0° 无缝阈值(无缝北极星不因移 GPU 破)。
TEST(TerrainBakeF32SeamTest, Float32PreservesSeamlessInvariant) {
    NeighborPair np;
    Source sA(np.a, np.field), sB(np.b, np.field);
    for (int gs : {64, 256}) {
        const int n = gs + 1;
        const auto fA = bake<float>(sA, np.a, gs);
        const auto fB = bake<float>(sB, np.b, gs);
        double maxSeam = 0;
        for (int j = 0; j < n; ++j)
            maxSeam = std::max(maxSeam, angleDeg(decodeNormal(fA, n, n - 1, j),
                                                 decodeNormal(fB, n, 0, j)));
        EXPECT_LT(maxSeam, kMaxSeamAngleDegrees)
            << "grid" << gs << ":f32 跨瓦共享边最大法线夹角 " << maxSeam << "°";
    }
}
