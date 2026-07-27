#pragma once

#include "SurfaceTile.h"
#include "TileKey.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace earth_engine {

struct TileFillGeometrySignature {
    static constexpr int kMaximumGridSize = 256;

    Rectangle bounds;
    RasterOverlayProjection projection =
        RasterOverlayProjection::Geographic;
    int gridSize = 1;
    // 代理顶点的高度来源(最近的带 heightmap 的地形祖先)。进签名是为了失效:
    // 加载暂态里祖先是陆续到达的,来源换了(或从"无"变成"有")就必须重建网格,
    // 否则代理会永远停在建它那一刻的高度 —— 通常是海平面。
    std::optional<TileKey> heightSourceKey;

    static std::optional<TileFillGeometrySignature> tryCreate(
        const Rectangle& bounds,
        RasterOverlayProjection projection,
        int gridSize,
        std::optional<TileKey> heightSourceKey = std::nullopt) {
        const bool finite =
            std::isfinite(bounds.west()) &&
            std::isfinite(bounds.south()) &&
            std::isfinite(bounds.east()) &&
            std::isfinite(bounds.north());
        const int effectiveGridSize = std::max(1, gridSize);
        if (!finite ||
            bounds.isEmpty() ||
            bounds.width() <= 0.0 ||
            bounds.height() <= 0.0 ||
            effectiveGridSize > kMaximumGridSize) {
            return std::nullopt;
        }
        return TileFillGeometrySignature{
            bounds,
            projection,
            effectiveGridSize,
            std::move(heightSourceKey)};
    }

    bool operator==(const TileFillGeometrySignature& other) const {
        return bounds == other.bounds &&
               projection == other.projection &&
               gridSize == other.gridSize &&
               heightSourceKey == other.heightSourceKey;
    }

    bool operator!=(const TileFillGeometrySignature& other) const {
        return !(*this == other);
    }
};

} // namespace earth_engine
