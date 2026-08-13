#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Gcj02CoordinateTransform.h"
#include "../data/VectorTileRasterizer.h"  // MercatorRect

namespace earth_engine::mvt_rect {

/// 页 key 矩形 → 数据瓦覆盖的共享数学(刀1 drape 与刀2 场烘焙同一套语义,
/// 从 VectorDrapeImageryProvider 提炼)。全部纯函数。

constexpr double kPi = 3.14159265358979323846;

inline MercatorRect tileToUnitRect(int z, int x, int y) {
    const double span = 1.0 / static_cast<double>(1ull << z);
    return MercatorRect{x * span, y * span, (x + 1) * span, (y + 1) * span};
}

inline double unitXFromLongitude(double lngRad) {
    return lngRad / (2.0 * kPi) + 0.5;
}

inline double unitYFromLatitude(double latRad) {
    return 0.5 - std::log(std::tan(kPi / 4.0 + latRad / 2.0)) / (2.0 * kPi);
}

inline double longitudeFromUnitX(double u) { return (u - 0.5) * 2.0 * kPi; }

inline double latitudeFromUnitY(double v) {
    return 2.0 * std::atan(std::exp(kPi * (1.0 - 2.0 * v))) - kPi / 2.0;
}

/// GCJ 页矩形 → 真实 WGS84 覆盖区(中心点 toWgs84 常量平移)。**仅用于选取
/// 覆盖的 OSM 源瓦片**(选瓦只需范围粗略够,±500m 偏移被 coverage 的边界
/// 向外取整吸收)。⚠️ **不再用于栅格化坐标映射** —— 那里整页单点平移在大页/
/// overzoom 祖先页边缘发散上百米(真机中景错位根因),改由逐顶点
/// wgsUnitToGcjUnit(见下)承载,与地形逐顶点 GCJ texcoord 同精度。
inline MercatorRect shiftRectGcjToWgs84(const MercatorRect& rect) {
    const double cu = 0.5 * (rect.x0 + rect.x1);
    const double cv = 0.5 * (rect.y0 + rect.y1);
    const Cartographic gcj = Cartographic::fromRadians(
        longitudeFromUnitX(cu), latitudeFromUnitY(cv));
    const Cartographic wgs = Gcj02CoordinateTransform::toWgs84(gcj);
    const double du = unitXFromLongitude(wgs.longitude()) - cu;
    const double dv = unitYFromLatitude(wgs.latitude()) - cv;
    return MercatorRect{rect.x0 + du, rect.y0 + dv, rect.x1 + du,
                        rect.y1 + dv};
}

/// 逐顶点 WGS84 unit-mercator → GCJ unit-mercator(fromWgs84)。作为
/// VectorTileRasterizer/LineFieldRasterizer 的 UnitTransform 传入:把每个 OSM
/// 顶点的真实位置搬到 GCJ 采样空间,与高德影像逐像素 GCJ + 地形逐顶点 GCJ
/// texcoord 完全对齐。逐点做,不受页大小影响(整页平移的发散在此消失)。
inline void wgsUnitToGcjUnit(double& u, double& v) {
    const Cartographic gcj = Gcj02CoordinateTransform::fromWgs84(
        Cartographic::fromRadians(longitudeFromUnitX(u), latitudeFromUnitY(v)));
    u = unitXFromLongitude(gcj.longitude());
    v = unitYFromLatitude(gcj.latitude());
}

struct TileXY {
    int x = 0;
    int y = 0;
};

/// 覆盖 rect 的 dataZ 级瓦片集合。右/下边界用 ceil-1:边界恰在瓦缝
/// (无 GCJ 平移时的常态)不多取一排。行主序枚举。
inline std::vector<TileXY> coverage(const MercatorRect& rect, int dataZ) {
    const int n = 1 << dataZ;
    const auto clampTile = [n](int v) {
        return std::max(0, std::min(v, n - 1));
    };
    const int tx0 = clampTile(static_cast<int>(std::floor(rect.x0 * n)));
    const int tx1 = clampTile(
        std::max(tx0, static_cast<int>(std::ceil(rect.x1 * n)) - 1));
    const int ty0 = clampTile(static_cast<int>(std::floor(rect.y0 * n)));
    const int ty1 = clampTile(
        std::max(ty0, static_cast<int>(std::ceil(rect.y1 * n)) - 1));
    std::vector<TileXY> out;
    out.reserve(static_cast<size_t>(tx1 - tx0 + 1) * (ty1 - ty0 + 1));
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            out.push_back(TileXY{tx, ty});
        }
    }
    return out;
}

} // namespace earth_engine::mvt_rect
