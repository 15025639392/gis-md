#pragma once

// Selector 对拍共享场景定义（见 DESIGN.md）。
// 本头文件必须保持自包含：只用标准库标量，不依赖 gis-md 或 cesium-native
// 的任何类型——cesium golden 生成器与 gis-md 单测都直接 include 它，
// 场景展开（树结构 / 相机 / 选项）由此单一来源保证两侧逐位一致。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace selector_diff {

struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// WGS84（与两侧引擎所用常量一致）
inline constexpr double kWgs84A = 6378137.0;
inline constexpr double kWgs84B = 6356752.314245179497563967;

// 标准大地坐标 → ECEF（两侧统一用这一份实现，避免各自 ellipsoid 代码
// 的实现差异进入对拍面）
inline Vec3d cartographicToEcef(double lonRad, double latRad, double height) {
    const double a2 = kWgs84A * kWgs84A;
    const double b2 = kWgs84B * kWgs84B;
    const double cosLat = std::cos(latRad);
    const double sinLat = std::sin(latRad);
    const double n =
        a2 / std::sqrt(a2 * cosLat * cosLat + b2 * sinLat * sinLat);
    return Vec3d{
        (n + height) * cosLat * std::cos(lonRad),
        (n + height) * cosLat * std::sin(lonRad),
        (n * (b2 / a2) + height) * sinLat};
}

inline Vec3d normalize(const Vec3d& v) {
    const double len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return Vec3d{v.x / len, v.y / len, v.z / len};
}

// nadir 方向 = 指向地心；up = 本地北在视平面上的投影（nadir 相机的
// 标准正交化 up，两侧统一）
inline Vec3d nadirDirection(const Vec3d& position) {
    return normalize(Vec3d{-position.x, -position.y, -position.z});
}

inline Vec3d nadirUpLocalNorth(const Vec3d& position) {
    const Vec3d dir = nadirDirection(position);
    // north = Z - (Z·dir)dir
    const double dz = dir.z;  // Z·dir，Z=(0,0,1)
    Vec3d north{-dz * dir.x, -dz * dir.y, 1.0 - dz * dir.z};
    return normalize(north);
}

// ---- 场景 S1：4 层满四叉树 + nadir 阶梯下降 ----

struct QuadtreeSpec {
    // 根 region（弧度）
    double west;
    double south;
    double east;
    double north;
    double rootGeometricError;  // geometricError(z) = root / 2^z
    int maxDepth;               // 根为 z=0，叶为 z=maxDepth
};

struct CameraFrameSpec {
    double lonRad;
    double latRad;
    double heightMeters;
};

struct OptionsSpec {
    double maximumScreenSpaceError = 16.0;
    bool enableFrustumCulling = true;
    bool enableFogCulling = true;
    bool enableOcclusionCulling = false;   // 遮挡不是对拍面（DESIGN 白名单 4）
    bool enableLodTransitionPeriod = false;
    bool forbidHoles = false;
    uint32_t loadingDescendantLimit = 20;
    // P1 设 100（S1 全树 85 瓦片，保证节流永不触顶——loads 对拍的是
    // "选择器想加载什么"而非分发节流；节流面 P2 再对拍）
    uint32_t maximumSimultaneousTileLoads = 100;
    bool preloadAncestors = true;
    bool preloadSiblings = true;
    bool enforceCulledScreenSpaceError = true;
    double culledScreenSpaceError = 64.0;
    double verticalFovRadians = 60.0 * 3.14159265358979323846 / 180.0;
    double viewportWidth = 1280.0;
    double viewportHeight = 720.0;
};

// fog 表（与 cesium-native TilesetOptions 默认 fogDensityTable 同源；
// 显式列出使两侧不依赖各自默认值）。{height(m), density}
struct FogEntry {
    double height;
    double density;
};

// 与 cesium-native TilesetOptions.h:216-223（bfc2c574c）逐字一致
inline constexpr std::array<FogEntry, 21> kFogTable{{
    {359.393, 2.0e-5},     {800.749, 2.0e-4},     {1275.6501, 1.0e-4},
    {2151.1192, 7.0e-5},   {3141.7763, 5.0e-5},   {4777.5198, 4.0e-5},
    {6281.2493, 3.0e-5},   {12364.307, 1.9e-5},   {15900.765, 1.0e-5},
    {49889.0549, 8.5e-6},  {78026.8259, 6.2e-6},  {99260.7344, 5.8e-6},
    {120036.3873, 5.3e-6}, {151011.0158, 5.2e-6}, {156091.1953, 5.1e-6},
    {203849.3112, 4.2e-6}, {274866.9803, 4.0e-6}, {319916.3149, 3.4e-6},
    {493552.0528, 2.6e-6}, {628733.5874, 2.2e-6}, {1000000.0, 0.0},
}};

// S1 根 region：赤道附近 π/16 × π/16（约 1250km 见方），避开极点/反经线
inline constexpr QuadtreeSpec kS1Tree{
    /*west=*/0.0,
    /*south=*/0.0,
    /*east=*/3.14159265358979323846 / 16.0,
    /*north=*/3.14159265358979323846 / 16.0,
    /*rootGeometricError=*/100000.0,
    /*maxDepth=*/3};

// S1 相机：region 中心上空 nadir 阶梯下降。
// SSE 参考（viewport 720 / fovY 60°）：refine 当 distance < ~39 × GE，
// 即 z0 需 d<3.9e6、z3 叶 d<4.9e5——序列覆盖"看根→看到叶"全程。
inline constexpr double kS1CenterLon =
    (kS1Tree.west + kS1Tree.east) / 2.0;
inline constexpr double kS1CenterLat =
    (kS1Tree.south + kS1Tree.north) / 2.0;

inline constexpr std::array<CameraFrameSpec, 12> kS1Frames{{
    {kS1CenterLon, kS1CenterLat, 8000000.0},
    {kS1CenterLon, kS1CenterLat, 8000000.0},  // 重复帧：收敛稳定性
    {kS1CenterLon, kS1CenterLat, 5000000.0},
    {kS1CenterLon, kS1CenterLat, 3000000.0},
    {kS1CenterLon, kS1CenterLat, 3000000.0},
    {kS1CenterLon, kS1CenterLat, 1500000.0},
    {kS1CenterLon, kS1CenterLat, 800000.0},
    {kS1CenterLon, kS1CenterLat, 800000.0},
    {kS1CenterLon, kS1CenterLat, 400000.0},
    {kS1CenterLon, kS1CenterLat, 200000.0},
    {kS1CenterLon, kS1CenterLat, 200000.0},
    {kS1CenterLon, kS1CenterLat, 200000.0},  // 尾帧重复：终态收敛
}};

inline constexpr OptionsSpec kS1Options{};

// ---- 子瓦片切分（两侧统一语义：dx=0 西半，dy=0 南半）----

struct RegionSpec {
    double west;
    double south;
    double east;
    double north;
};

// 父 region 等分四切。子 key = (z+1, 2x+dx, 2y+dy)，dx∈{0,1} 西→东，
// dy∈{0,1} 南→北。
inline RegionSpec childRegion(const RegionSpec& parent, int dx, int dy) {
    const double midLon = (parent.west + parent.east) / 2.0;
    const double midLat = (parent.south + parent.north) / 2.0;
    return RegionSpec{
        dx == 0 ? parent.west : midLon,
        dy == 0 ? parent.south : midLat,
        dx == 0 ? midLon : parent.east,
        dy == 0 ? midLat : parent.north};
}

inline double geometricErrorForLevel(const QuadtreeSpec& tree, int z) {
    return tree.rootGeometricError / static_cast<double>(1 << z);
}

// ---- 轨迹行格式（两侧 driver 都用这一份，保证 byte 级一致）----
//   frame=N render=[a,b,...] loads=[c,...] visited=V culled=C culledVisited=CV kicked=K
// 瓦片 key 统一 "z-x-y"；render/loads 排序后输出。

inline std::string tileKeyString(int z, int x, int y) {
    return std::to_string(z) + "-" + std::to_string(x) + "-" +
           std::to_string(y);
}

inline std::string joinSorted(std::vector<std::string> keys) {
    std::sort(keys.begin(), keys.end());
    std::string out;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += keys[i];
    }
    return out;
}

inline std::string traceLine(
    int frame,
    std::vector<std::string> renderKeys,
    std::vector<std::string> loadKeys,
    long visited,
    long culled,
    long culledVisited,
    long kicked) {
    return "frame=" + std::to_string(frame) + " render=[" +
           joinSorted(std::move(renderKeys)) + "] loads=[" +
           joinSorted(std::move(loadKeys)) + "] visited=" +
           std::to_string(visited) + " culled=" + std::to_string(culled) +
           " culledVisited=" + std::to_string(culledVisited) +
           " kicked=" + std::to_string(kicked);
}

}  // namespace selector_diff
