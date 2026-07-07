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

// 带俯仰的相机基（pitch=0 时退化为 nadir + local-north up，与旧函数
// 逐位一致；两侧 driver 对所有场景统一使用这一对函数）：
//   dir(p) = down·cos(p) + north·sin(p)
//   up(p)  = north·cos(p) − down·sin(p)
inline Vec3d pitchedDirection(const Vec3d& position, double pitchRad) {
    const Vec3d down = nadirDirection(position);
    const Vec3d north = nadirUpLocalNorth(position);
    const double c = std::cos(pitchRad);
    const double s = std::sin(pitchRad);
    return normalize(Vec3d{
        down.x * c + north.x * s,
        down.y * c + north.y * s,
        down.z * c + north.z * s});
}

inline Vec3d pitchedUp(const Vec3d& position, double pitchRad) {
    const Vec3d down = nadirDirection(position);
    const Vec3d north = nadirUpLocalNorth(position);
    const double c = std::cos(pitchRad);
    const double s = std::sin(pitchRad);
    return normalize(Vec3d{
        north.x * c - down.x * s,
        north.y * c - down.y * s,
        north.z * c - down.z * s});
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
    // 从 nadir 向本地北方地平线的俯仰（弧度）。0 = 正俯视（nadir）。
    // 所有场景统一用 pitchedDirection/pitchedUp 构造相机。
    double pitchRad = 0.0;
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
//
// 相机经度整体偏离 region 中心 +0.0037 rad（~13'）：打破东西镜像对称，
// 消灭精确并列的加载优先级对——并列项的排序顺序是 stdlib introsort 的
// 产物（无语义、跨实现可变），loads 保序对拍必须在场景层面避开
// （DESIGN 白名单 7）。
inline constexpr double kS1CenterLon =
    (kS1Tree.west + kS1Tree.east) / 2.0 + 0.0037;
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

// ---- 场景 S2：冷启动深缩放 + 低 loadingDescendantLimit 触发 restore/kick ----
//
// 相机从第 0 帧起即位于 region 中心上空 400km（需要 z3 叶，视锥内可见
// ~24 个叶瓦片），全树 Unloaded 起步：not-yet-renderable 后代数远超
// loadingDescendantLimit=8（条件为严格大于；首轮 200km 方案恰好 8 个
// 可见叶差一不触发，故抬到 400km 远离边界），祖先必然走"restore 子孙
// load queue + 改载父瓦片"分支（cesium TilesetSelection.cpp:748-771），
// golden 的 kicked 计数非零——差分覆盖 kick/restore 面（2026-07-04 对齐
// 的 kicked 恢复条数语义）。加载模型与 S1 相同（帧 N 请求 → 帧 N+1 可
// 渲染），restore 使每帧只放行一层，收敛逐级向下、kick 连续多帧非零。

inline constexpr std::array<CameraFrameSpec, 12> kS2Frames{{
    {kS1CenterLon, kS1CenterLat, 400000.0},
    {kS1CenterLon, kS1CenterLat, 400000.0},
    {kS1CenterLon, kS1CenterLat, 400000.0},
    {kS1CenterLon, kS1CenterLat, 400000.0},
    {kS1CenterLon, kS1CenterLat, 400000.0},
    {kS1CenterLon, kS1CenterLat, 400000.0},
    {kS1CenterLon, kS1CenterLat, 400000.0},
    {kS1CenterLon, kS1CenterLat, 400000.0},
    {kS1CenterLon, kS1CenterLat, 400000.0},
    {kS1CenterLon, kS1CenterLat, 400000.0},
    {kS1CenterLon, kS1CenterLat, 400000.0},
    {kS1CenterLon, kS1CenterLat, 400000.0},
}};

inline const OptionsSpec kS2Options = [] {
    OptionsSpec options{};
    options.loadingDescendantLimit = 8;
    return options;
}();

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

// ---- 场景 S3：加载延迟脚本（多帧渐进 restore/kick）----
//
// 与 S2 同相机（400km 冷启动）同选项（limit=8），但加载模型改为
// kS3LoadDelayFrames=2：**帧 N 发起的请求在帧 N+D 的选择开始前完成**
// （S1/S2 是 D=1 的次帧模型）。root 帧 0 restore（kicked>0），帧 1
// root 仍不可渲染 → 再次 restore（kicked 再次非零）——kick/restore 面
// 从单帧扩展到多帧渐进收敛，覆盖 wasReallyRenderedLastFrame 时序。
//
// 两侧时序契约（每帧）：
//   1. 完成"发起帧 + D <= 当前帧"的请求（cesium：resolve promise 后
//      dispatchMainThreadTasks；gis-md：置 loadState=Done）
//   2. 选择（updateViewGroup / selectTiles）
//   3. 发起本帧请求并记录（loads 输出）

inline constexpr int kS3LoadDelayFrames = 2;
inline constexpr const std::array<CameraFrameSpec, 12>& kS3Frames =
    kS2Frames;
inline const OptionsSpec& kS3Options = kS2Options;

// ---- 场景 S4：fog 边界（低空倾斜相机看地平线）----
//
// cesium 的 fog 剔除条件是 exp(-(d·ρ)²) == 0.0（double 下溢），需要
// d·ρ ≳ 27.3：nadir 相机低空视锥内没有那么远的瓦片（首版方案证伪，
// 与关 fog 对照 byte 级一致）。改为：相机在 region 南缘内侧低空、
// 80° 俯仰朝北看地平线——region 纵深 ~1250km，北部远瓦片入锥。
// 各高度的可达性（ρ 取 fog 表插值，d 需 ≥ 27.3/ρ）：
//   800m: ρ=2.0e-4 → d≥137km ✓   3km: ρ=5e-5 → d≥546km ✓
//   6km:  ρ=3e-5  → d≥910km ✓(临界)   12km: ρ=1.9e-5 → d≥1437km ✗
// ——序列穿越"必然剔除 / 临界 / 不可达"三段，覆盖 computeFogDensity
// 插值与剔除判定边界。cesium 把 fog 剔除计入 tilesCulled（无独立
// 计数），gis-md 侧 trace 的 culled 字段映射为 frustum+fog 之和。

inline constexpr double kS4Pitch = 80.0 * 3.14159265358979323846 / 180.0;
inline constexpr double kS4Lat = kS1Tree.south + 0.008;  // 南缘内侧 ~51km

inline constexpr std::array<CameraFrameSpec, 12> kS4Frames{{
    {kS1CenterLon, kS4Lat, 800.0, kS4Pitch},
    {kS1CenterLon, kS4Lat, 800.0, kS4Pitch},
    {kS1CenterLon, kS4Lat, 1500.0, kS4Pitch},
    {kS1CenterLon, kS4Lat, 3000.0, kS4Pitch},
    {kS1CenterLon, kS4Lat, 3000.0, kS4Pitch},
    {kS1CenterLon, kS4Lat, 6000.0, kS4Pitch},
    {kS1CenterLon, kS4Lat, 6000.0, kS4Pitch},
    {kS1CenterLon, kS4Lat, 12000.0, kS4Pitch},
    {kS1CenterLon, kS4Lat, 12000.0, kS4Pitch},
    {kS1CenterLon, kS4Lat, 50000.0, kS4Pitch},
    {kS1CenterLon, kS4Lat, 120000.0, kS4Pitch},
    {kS1CenterLon, kS4Lat, 320000.0, kS4Pitch},
}};

inline constexpr OptionsSpec kS4Options{};

// ---- 场景 S5：等高横向平移（pan）——增量切面机会测量的关键场景 ----
//
// 真机拖动是横向 orbit-pan（相机绕地心侧移，LOD 近恒定），而非 S1 的纵向
// zoom。zoom 每帧换 LOD 壳 → 全子树 churn（S1 实测 0% stable）；pan 保持
// LOD 恒定，只有视锥前/后缘瓦片进出 → 内部子树应大量稳定。此场景量化 pan
// 下的 stable 子树比例 = ③ L3 剪枝对"拖动 selector 成本"的真实上界（zoom 0%
// 与 still 100% 之间的那个未知数）。
// 定高 400km（z3 叶可见、含 z2 内部子树），前 3 帧收敛，其后每帧东移
// +0.008 rad（~51km，约半个 z3 瓦片宽），始终留在 region [0, π/16] 内，
// 两侧都有数据。nadir（pitch=0）。§8 oracle 逐帧验证增量==全量。
inline constexpr std::array<CameraFrameSpec, 12> kS5Frames{{
    {0.060, 0.098, 400000.0},
    {0.060, 0.098, 400000.0},
    {0.060, 0.098, 400000.0},
    {0.068, 0.098, 400000.0},
    {0.076, 0.098, 400000.0},
    {0.084, 0.098, 400000.0},
    {0.092, 0.098, 400000.0},
    {0.100, 0.098, 400000.0},
    {0.108, 0.098, 400000.0},
    {0.116, 0.098, 400000.0},
    {0.124, 0.098, 400000.0},
    {0.132, 0.098, 400000.0},
}};

inline constexpr OptionsSpec kS5Options{};

// ---- 轨迹行格式（两侧 driver 都用这一份，保证 byte 级一致）----
//   frame=N render=[a,b,...] loads=[c,...] visited=V culled=C culledVisited=CV kicked=K
// 瓦片 key 统一 "z-x-y"。render 排序输出（遍历序不作为对拍面）；
// loads **保序输出**（P3 起）：两侧都按传输/分发层实际发起顺序——
// cesium = mock loader 被调用顺序（load queue 按 group/priority 排序后
// 依次发起），gis-md = 生产分发排序后的顺序。这使加载优先级公式与
// 排序语义成为对拍面。

inline std::string tileKeyString(int z, int x, int y) {
    return std::to_string(z) + "-" + std::to_string(x) + "-" +
           std::to_string(y);
}

inline std::string joinList(const std::vector<std::string>& keys) {
    std::string out;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += keys[i];
    }
    return out;
}

inline std::string joinSorted(std::vector<std::string> keys) {
    std::sort(keys.begin(), keys.end());
    return joinList(keys);
}

inline std::string traceLine(
    int frame,
    std::vector<std::string> renderKeys,
    std::vector<std::string> loadKeysInIssueOrder,
    long visited,
    long culled,
    long culledVisited,
    long kicked) {
    return "frame=" + std::to_string(frame) + " render=[" +
           joinSorted(std::move(renderKeys)) + "] loads=[" +
           joinList(loadKeysInIssueOrder) + "] visited=" +
           std::to_string(visited) + " culled=" + std::to_string(culled) +
           " culledVisited=" + std::to_string(culledVisited) +
           " kicked=" + std::to_string(kicked);
}

}  // namespace selector_diff
