#pragma once

#include "../core/math/Rectangle.h"
#include "../tiling/TileKey.h"

#include <cstdint>
#include <vector>

namespace earth_engine {

class ImageryProvider;
class TileScheme;

/// Immutable geometry-to-imagery source plan shared by raster backends.
///
/// The plan contains only coordinate-domain and source-selection decisions.
/// It intentionally has no provider cache, request, upload, or attachment
/// state so Direct and PageStore executors can consume the same result.
struct RasterOverlaySourcePlan {
    int sourceZoom = 0;
    Rectangle sourceBounds = Rectangle::MAXIMUM;
    std::vector<TileKey> sourceKeys;
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
    bool exactSingleSource = false;

    int budgetUnits() const {
        return static_cast<int>(sourceKeys.size());
    }

    bool empty() const {
        return sourceKeys.empty();
    }
};

/// Source tile range after antimeridian splitting and Cesium boundary-slop
/// trimming. Public for backend-neutral diagnostics and pure-function tests.
struct RasterOverlaySourceRange {
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;

    int width() const;
    int height() const;
    int count() const;
    int64_t width64() const;
    int64_t height64() const;
    int64_t count64() const;
};

struct RasterOverlaySourceCoverage {
    std::vector<RasterOverlaySourceRange> ranges;

    int width() const;
    int height() const;
    int count() const;
    int64_t width64() const;
    int64_t height64() const;
    int64_t count64() const;
    RasterOverlaySourceRange combinedRange() const;
};

/// Enumerate source coverage using the provider scheme. Antimeridian bounds
/// are represented as two ranges; edge-touching tiles are excluded with the
/// Cesium-native 1/512 geometry-span tolerance.
RasterOverlaySourceCoverage enumerateRasterOverlaySourceCoverage(
    const TileScheme& scheme,
    const Rectangle& bounds,
    int zoom);

/// Select a source zoom from target screen pixels/MSE and clamp it so the
/// resulting source coverage fits within maximumTextureSize.
int chooseRasterOverlaySourceZoom(
    const TileScheme& scheme,
    const ImageryProvider& provider,
    const Rectangle& geometryBounds,
    const Rectangle& sourceBounds,
    double targetScreenPixelsX,
    double targetScreenPixelsY,
    double maximumScreenSpaceError,
    int maximumTextureSize,
    int minimumLevel,
    int maximumLevel,
    RasterOverlaySourceCoverage* selectedCoverage = nullptr);

/// Build the complete immutable source plan consumed by raster executors.
RasterOverlaySourcePlan buildRasterOverlaySourcePlan(
    const TileScheme& scheme,
    const ImageryProvider& provider,
    const Rectangle& geometryBounds,
    const Rectangle& sourceBounds,
    double targetScreenPixelsX,
    double targetScreenPixelsY,
    double maximumScreenSpaceError,
    int maximumTextureSize,
    int minimumLevel,
    int maximumLevel);

} // namespace earth_engine
