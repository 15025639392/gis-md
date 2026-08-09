#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/TerrainDisplacementTemplatePool.h"
#include "earth_engine/tiling/TileScheme.h"

using namespace earth_engine;

namespace {

constexpr double kPi = 3.14159265358979323846;

// 连续解析高度场。两块相邻瓦片各自采样它,故"同一物理点高度相同"按构造成立
// —— 边界法线若不一致,只可能来自烘焙侧的差分模板,不可能来自数据。
//
// 两处刻意的设计,少任何一处这条测试都会**在缺陷仍在时通过**:
// ① 高频项(每瓦 16 周期,z8 下 ≈9.75km 波长 / 300m 起伏,真实山地量级)。
//    只有低频项时曲率相对网格步长太小,单边与中心差分之差落进量化噪声底。
// ② 相位偏移。不加的话瓦片边界恰好落在 sin 的过零点(f''=0),而单边/中心
//    差分之差正比于二阶导 —— 缺陷被测试场自己完美掩盖(实测 0.45°=1 LSB)。
struct HeightField {
    double kx = 0.0;
    double ky = 0.0;
    // 0 = 只留低频光滑项。内部法线与解析梯度的对拍用光滑场:高频项在
    // grid64 下每周期只有 4 个网格点,离散化误差本身就有几度,会淹没
    // "估计量是否无偏"这个真正要钉的性质。
    double roughAmplitude = 300.0;

    double operator()(double lon, double lat) const {
        const double hx = kx * 64.0;  // 4×16:每瓦片 16 个周期
        const double hy = ky * 64.0;
        return 1200.0 * std::sin(kx * lon) * std::cos(ky * lat) +
               400.0 * std::sin(3.0 * kx * lon) +
               300.0 * std::cos(2.0 * ky * lat) +
               roughAmplitude * std::sin(hx * lon + 0.7853981634) *
                   std::cos(hy * lat + 0.3926990817);
    }
};

// cell-registered + 1px 重叠环的 514 结构(生产全球 Terrain-RGB 源的形状):
// 512 个 cell 中心落在像素 1..512,瓦片真实边界在半像素 0.5 / 512.5,像素 0 与
// 513 是邻居的重叠 backfill。像素高度直接取解析场 → 重叠环天然携带真实邻居值。
DecodedHeightmap makeRingHeightmap(const Rectangle& bounds,
                                   const HeightField& field) {
    constexpr int kTileSize = 514;
    constexpr float kInset = 0.5f;
    const double span = static_cast<double>(kTileSize - 1) - 2.0 * kInset;

    DecodedHeightmap hm;
    hm.tileSize = kTileSize;
    hm.borderInset = kInset;
    hm.stagedHeights.resize(static_cast<size_t>(kTileSize) * kTileSize);

    float minH = 1e30f;
    float maxH = -1e30f;
    for (int py = 0; py < kTileSize; ++py) {
        const double v = (static_cast<double>(py) - kInset) / span;
        const double lat = bounds.north() - v * bounds.height();
        for (int px = 0; px < kTileSize; ++px) {
            const double u = (static_cast<double>(px) - kInset) / span;
            const double lon = bounds.west() + u * bounds.width();
            const float h = static_cast<float>(field(lon, lat));
            hm.stagedHeights[static_cast<size_t>(py) * kTileSize + px] = h;
            minH = std::min(minH, h);
            maxH = std::max(maxH, h);
        }
    }
    hm.assignHeights();
    hm.minHeight = minH;
    hm.maxHeight = maxH;
    return hm;
}

struct Normal {
    double x = 0.0;
    double y = 0.0;
    double z = 1.0;
};

// B/A 通道解码回切空间法线(shader 侧同一约定:只存 xy,nz>0 重建)。
Normal decodeNormal(const std::vector<uint8_t>& texels, int n, int i, int j) {
    const size_t idx = (static_cast<size_t>(j) * n + i) * 4;
    const auto dec = [](uint8_t b) {
        return static_cast<double>(b) / 255.0 * 2.0 - 1.0;
    };
    Normal out;
    out.x = dec(texels[idx + 2]);
    out.y = dec(texels[idx + 3]);
    const double zz = 1.0 - out.x * out.x - out.y * out.y;
    out.z = zz > 0.0 ? std::sqrt(zz) : 0.0;
    return out;
}

double angleBetweenDegrees(const Normal& a, const Normal& b) {
    const double dot = a.x * b.x + a.y * b.y + a.z * b.z;
    return std::acos(std::clamp(dot, -1.0, 1.0)) * 180.0 / kPi;
}

// 两个几何密度档都要盯:coarse(64)是远景主力,dense(256)是近景。
const int kGridSizes[] = {64, 256};

// 判据阈值。噪声底是 8bit 法线量化的 1 LSB ≈ 0.45°;本场景实测残差
// 1.50°(grid64)/ 1.52°(grid256),留到 2.0° 收口。
//
// 修复前同一场景是 **25.5°**(grid64)/ 7.7°(grid256)—— 阈值不是照着
// 改后数字放宽出来的,25° 与 1.5° 之间没有"调阈值"的空间。
//
// 残差来源:相邻两瓦在共享边界上用的是**互为镜像**的臂长(一侧长臂朝内、
// 短臂踩重叠环,另一侧反之)。三点公式对二次场精确,故残差是 O(d_l·d_r·f''')。
// 要归零需要重叠环宽度 ≥ 一个网格步长(现在是 0.5px vs 8px),那是数据侧
// 属性(切瓦时多带几圈环),不是引擎侧能单方面消掉的。
constexpr double kMaxSeamAngleDegrees = 2.0;

} // namespace

// 东西相邻两瓦片在共享经线上的法线必须一致 —— 否则每条瓦片边一道明暗带。
TEST(TerrainEdgeNormalSeamTest, EastWestNeighborsAgreeOnSharedEdge) {
    auto scheme = TileScheme::createGeographicTMS();
    constexpr int kZ = 8;
    constexpr int kX = 100;
    constexpr int kY = 60;
    const Rectangle boundsA =
        scheme->tileToRectangle(TileKey{"Geographic-TMS", kZ, kX, kY});
    const Rectangle boundsB =
        scheme->tileToRectangle(TileKey{"Geographic-TMS", kZ, kX + 1, kY});
    ASSERT_NEAR(boundsA.east(), boundsB.west(), 1e-12);

    HeightField field;
    field.kx = 2.0 * kPi / (4.0 * boundsA.width());
    field.ky = 2.0 * kPi / (4.0 * boundsA.height());

    const DecodedHeightmap hmA = makeRingHeightmap(boundsA, field);
    const DecodedHeightmap hmB = makeRingHeightmap(boundsB, field);

    for (int gridSize : kGridSizes) {
        const int n = gridSize + 1;
        const std::vector<uint8_t> texA = bakeTerrainHeightNormalTexels(
            hmA, boundsA, gridSize, hmA.minHeight,
            hmA.maxHeight - hmA.minHeight);
        const std::vector<uint8_t> texB = bakeTerrainHeightNormalTexels(
            hmB, boundsB, gridSize, hmB.minHeight,
            hmB.maxHeight - hmB.minHeight);

        // A 的东边列(i = n-1, u=1)与 B 的西边列(i = 0, u=0)是同一条经线。
        double maxAngle = 0.0;
        int worstRow = -1;
        for (int j = 0; j < n; ++j) {
            const double angle = angleBetweenDegrees(
                decodeNormal(texA, n, n - 1, j), decodeNormal(texB, n, 0, j));
            if (angle > maxAngle) {
                maxAngle = angle;
                worstRow = j;
            }
        }
        EXPECT_LT(maxAngle, kMaxSeamAngleDegrees)
            << "grid" << gridSize << ":共享经线上法线最大夹角 " << maxAngle
            << "° (行 " << worstRow
            << ") —— 相邻瓦片对同一物理点算出了不同法线,这就是瓦片边明暗带。";
    }
}

// 南北相邻同理:A 的南边行与 B 的北边行是同一条纬线。
TEST(TerrainEdgeNormalSeamTest, NorthSouthNeighborsAgreeOnSharedEdge) {
    auto scheme = TileScheme::createGeographicTMS();
    constexpr int kZ = 8;
    constexpr int kX = 100;
    constexpr int kY = 60;
    const Rectangle boundsNorth =
        scheme->tileToRectangle(TileKey{"Geographic-TMS", kZ, kX, kY});
    const Rectangle boundsSouth =
        scheme->tileToRectangle(TileKey{"Geographic-TMS", kZ, kX, kY - 1});
    ASSERT_NEAR(boundsNorth.south(), boundsSouth.north(), 1e-12);

    HeightField field;
    field.kx = 2.0 * kPi / (4.0 * boundsNorth.width());
    field.ky = 2.0 * kPi / (4.0 * boundsNorth.height());

    const DecodedHeightmap hmN = makeRingHeightmap(boundsNorth, field);
    const DecodedHeightmap hmS = makeRingHeightmap(boundsSouth, field);

    for (int gridSize : kGridSizes) {
        const int n = gridSize + 1;
        const std::vector<uint8_t> texN = bakeTerrainHeightNormalTexels(
            hmN, boundsNorth, gridSize, hmN.minHeight,
            hmN.maxHeight - hmN.minHeight);
        const std::vector<uint8_t> texS = bakeTerrainHeightNormalTexels(
            hmS, boundsSouth, gridSize, hmS.minHeight,
            hmS.maxHeight - hmS.minHeight);

        double maxAngle = 0.0;
        int worstCol = -1;
        for (int i = 0; i < n; ++i) {
            const double angle = angleBetweenDegrees(
                decodeNormal(texN, n, i, n - 1), decodeNormal(texS, n, i, 0));
            if (angle > maxAngle) {
                maxAngle = angle;
                worstCol = i;
            }
        }
        EXPECT_LT(maxAngle, kMaxSeamAngleDegrees)
            << "grid" << gridSize << ":共享纬线上法线最大夹角 " << maxAngle
            << "° (列 " << worstCol << ")。";
    }
}

// 手术式改动的不变量:重叠环只影响**边界**节点。把环像素涂成垃圾后,内部
// 节点的 texel 必须逐字节不变 —— 否则说明新差分把内部也改了(内部本来就是
// 等臂中心差分,三点公式在等臂下逐位退化为它)。
TEST(TerrainEdgeNormalSeamTest, RingOnlyAffectsBoundaryNodes) {
    auto scheme = TileScheme::createGeographicTMS();
    const Rectangle bounds =
        scheme->tileToRectangle(TileKey{"Geographic-TMS", 8, 100, 60});
    HeightField field;
    field.kx = 2.0 * kPi / (4.0 * bounds.width());
    field.ky = 2.0 * kPi / (4.0 * bounds.height());

    const DecodedHeightmap clean = makeRingHeightmap(bounds, field);
    DecodedHeightmap poisoned = clean;
    const int ts = poisoned.tileSize;
    for (int p = 0; p < ts; ++p) {
        for (int q : {0, ts - 1}) {
            poisoned.stagedHeights[static_cast<size_t>(q) * ts + p] = -1234.0f;
            poisoned.stagedHeights[static_cast<size_t>(p) * ts + q] = -1234.0f;
            poisoned.assignHeights();
        }
    }
    // minHeight/maxHeight 保持 clean 的,否则高度量化整体位移、比较失去意义。
    poisoned.minHeight = clean.minHeight;
    poisoned.maxHeight = clean.maxHeight;

    for (int gridSize : kGridSizes) {
        const int n = gridSize + 1;
        const auto a = bakeTerrainHeightNormalTexels(
            clean, bounds, gridSize, clean.minHeight,
            clean.maxHeight - clean.minHeight);
        const auto b = bakeTerrainHeightNormalTexels(
            poisoned, bounds, gridSize, clean.minHeight,
            clean.maxHeight - clean.minHeight);
        // 从第 2 圈起查:第 1 圈节点的**差分臂**够到边界节点,而边界节点
        // 自身位于 u=0(像素 0.5),其双线性本来就吃一半环像素 —— 这是改动前
        // 就有的正确行为(环是真实邻瓦数据,边界点的值本该由它参与)。
        for (int j = 2; j < n - 2; ++j) {
            for (int i = 2; i < n - 2; ++i) {
                const size_t idx = (static_cast<size_t>(j) * n + i) * 4;
                for (int c = 0; c < 4; ++c) {
                    ASSERT_EQ(a[idx + c], b[idx + c])
                        << "grid" << gridSize << " 内部节点 (" << i << "," << j
                        << ") 通道 " << c << " 被重叠环影响了";
                }
            }
        }
    }
}

// 对照:内部节点不受边界退化影响,烘出的法线应贴合解析场的真实梯度。
// 这条钉住"差分本身是对的",避免把边界修法误改成整体偏移。
TEST(TerrainEdgeNormalSeamTest, InteriorNormalsMatchAnalyticGradient) {
    auto scheme = TileScheme::createGeographicTMS();
    const Rectangle bounds =
        scheme->tileToRectangle(TileKey{"Geographic-TMS", 8, 100, 60});
    HeightField field;
    field.kx = 2.0 * kPi / (4.0 * bounds.width());
    field.ky = 2.0 * kPi / (4.0 * bounds.height());
    field.roughAmplitude = 0.0;

    const DecodedHeightmap hm = makeRingHeightmap(bounds, field);
    constexpr int kGridSize = 64;
    const int n = kGridSize + 1;
    const std::vector<uint8_t> tex = bakeTerrainHeightNormalTexels(
        hm, bounds, kGridSize, hm.minHeight, hm.maxHeight - hm.minHeight);

    constexpr double kEarthRadiusMeters = 6378137.0;
    const double centerLat = 0.5 * (bounds.north() + bounds.south());
    const double widthMeters =
        bounds.width() * std::cos(centerLat) * kEarthRadiusMeters;
    const double heightSpanMeters = bounds.height() * kEarthRadiusMeters;

    double maxAngle = 0.0;
    for (int j = 8; j < n - 8; j += 8) {
        for (int i = 8; i < n - 8; i += 8) {
            const double u = static_cast<double>(i) / kGridSize;
            const double v = static_cast<double>(j) / kGridSize;
            const double lon = bounds.west() + u * bounds.width();
            const double lat = bounds.north() - v * bounds.height();
            // 解析梯度(米/米)。v 向南为正,故 lat 方向取负号。
            const double dLon = bounds.width() * 1e-4;
            const double dLat = bounds.height() * 1e-4;
            const double gradU = (field(lon + dLon, lat) - field(lon - dLon, lat)) /
                                 (2.0 * dLon / bounds.width() * widthMeters);
            const double gradV = (field(lon, lat - dLat) - field(lon, lat + dLat)) /
                                 (2.0 * dLat / bounds.height() * heightSpanMeters);
            const double inv =
                1.0 / std::sqrt(gradU * gradU + gradV * gradV + 1.0);
            Normal expected;
            expected.x = -gradU * inv;
            expected.y = -gradV * inv;
            expected.z = inv;
            maxAngle = std::max(
                maxAngle, angleBetweenDegrees(decodeNormal(tex, n, i, j), expected));
        }
    }
    // 容差覆盖 8bit 法线量化(~0.45°/LSB)与栅格差分对解析梯度的离散误差。
    EXPECT_LT(maxAngle, 3.0) << "内部法线与解析梯度最大夹角 " << maxAngle << "°";
}
