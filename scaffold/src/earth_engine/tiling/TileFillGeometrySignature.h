#pragma once

#include "SurfaceTile.h"

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

    static std::optional<TileFillGeometrySignature> tryCreate(
        const Rectangle& bounds,
        RasterOverlayProjection projection,
        int gridSize) {
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
            effectiveGridSize};
    }

    bool operator==(const TileFillGeometrySignature& other) const {
        return bounds == other.bounds &&
               projection == other.projection &&
               gridSize == other.gridSize;
    }

    bool operator!=(const TileFillGeometrySignature& other) const {
        return !(*this == other);
    }
};

} // namespace earth_engine
