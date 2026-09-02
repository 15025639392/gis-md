#include "RasterOverlaySourcePlan.h"

#include "ImageryProvider.h"
#include "RasterOverlayImageCompositing.h"
#include "../tiling/TileScheme.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>

namespace earth_engine {
namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr double kTwoPi = 2.0 * kPi;
constexpr int64_t kMaximumSourcePlanReserve = 1'000'000;
constexpr int kMaximumTextureSizeFallback = 2048;

RasterOverlaySourceRange computeRange(const TileScheme& scheme,
                                      const Rectangle& bounds,
                                      int zoom) {
    RasterOverlaySourceRange range;
    scheme.tileRange(bounds,
                     zoom,
                     range.minX,
                     range.minY,
                     range.maxX,
                     range.maxY);
    if (range.maxX < range.minX) std::swap(range.maxX, range.minX);
    if (range.maxY < range.minY) std::swap(range.maxY, range.minY);
    return range;
}

RasterOverlaySourceRange trimBoundarySlop(
    const TileScheme& scheme,
    const Rectangle& geometryBounds,
    int zoom,
    RasterOverlaySourceRange range) {
    if (range.maxX < range.minX || range.maxY < range.minY) {
        return range;
    }

    // cesium-native QuadtreeRasterOverlayTileProvider excludes tiles that only
    // touch a geometry rectangle along a tile edge, using 1/512 of the
    // geometry span as the edge tolerance.
    const bool projectedWebMercator = scheme.crsProfile() == "EPSG:3857";
    auto projectedY = [projectedWebMercator](double latitude) {
        return projectedWebMercator ? webMercatorY(latitude) : latitude;
    };
    auto projectedSouthEdge = [&projectedY](const Rectangle& rectangle) {
        return projectedY(rectangle.south());
    };
    auto projectedNorthEdge = [&projectedY](const Rectangle& rectangle) {
        return projectedY(rectangle.north());
    };

    const double geometrySouth = projectedSouthEdge(geometryBounds);
    const double geometryNorth = projectedNorthEdge(geometryBounds);
    const double veryCloseX = std::max(1e-12, geometryBounds.width()) / 512.0;
    const double veryCloseY =
        std::max(1e-12, std::abs(geometryNorth - geometrySouth)) / 512.0;

    const Rectangle westTile = scheme.tileToRectangle(
        TileKey{scheme.id(), zoom, range.minX, range.minY});
    if (std::abs(westTile.east() - geometryBounds.west()) < veryCloseX &&
        range.minX < range.maxX) {
        ++range.minX;
    }

    const Rectangle eastTile = scheme.tileToRectangle(
        TileKey{scheme.id(), zoom, range.maxX, range.maxY});
    if (std::abs(eastTile.west() - geometryBounds.east()) < veryCloseX &&
        range.maxX > range.minX) {
        --range.maxX;
    }

    const bool yDown = scheme.yDirection().find("down") != std::string::npos;
    if (yDown) {
        const Rectangle northTile = scheme.tileToRectangle(
            TileKey{scheme.id(), zoom, range.minX, range.minY});
        if (std::abs(projectedSouthEdge(northTile) - geometryNorth) < veryCloseY &&
            range.minY < range.maxY) {
            ++range.minY;
        }

        const Rectangle southTile = scheme.tileToRectangle(
            TileKey{scheme.id(), zoom, range.maxX, range.maxY});
        if (std::abs(projectedNorthEdge(southTile) - geometrySouth) < veryCloseY &&
            range.maxY > range.minY) {
            --range.maxY;
        }
    } else {
        const Rectangle southTile = scheme.tileToRectangle(
            TileKey{scheme.id(), zoom, range.minX, range.minY});
        if (std::abs(projectedNorthEdge(southTile) - geometrySouth) < veryCloseY &&
            range.minY < range.maxY) {
            ++range.minY;
        }

        const Rectangle northTile = scheme.tileToRectangle(
            TileKey{scheme.id(), zoom, range.maxX, range.maxY});
        if (std::abs(projectedSouthEdge(northTile) - geometryNorth) < veryCloseY &&
            range.maxY > range.minY) {
            --range.maxY;
        }
    }
    return range;
}

int64_t saturatingPixelSpan(int64_t tiles, int tilePixels) {
    if (tiles <= 0 || tilePixels <= 0) return 0;
    const int64_t pixels = static_cast<int64_t>(tilePixels);
    if (tiles > std::numeric_limits<int64_t>::max() / pixels) {
        return std::numeric_limits<int64_t>::max();
    }
    return tiles * pixels;
}

bool rectanglesOverlapWithArea(const Rectangle& a, const Rectangle& b) {
    const auto overlapsNonCrossing = [](const Rectangle& lhs,
                                        const Rectangle& rhs) {
        const std::optional<Rectangle> intersection =
            lhs.computeIntersection(rhs);
        return intersection && intersection->width() > 1e-15 &&
               intersection->height() > 1e-15;
    };
    const auto aParts = a.splitAtAntimeridian();
    const auto bParts = b.splitAtAntimeridian();
    if (overlapsNonCrossing(aParts.first, bParts.first)) return true;
    if (aParts.second && overlapsNonCrossing(*aParts.second, bParts.first)) {
        return true;
    }
    if (bParts.second && overlapsNonCrossing(aParts.first, *bParts.second)) {
        return true;
    }
    return aParts.second && bParts.second &&
           overlapsNonCrossing(*aParts.second, *bParts.second);
}

struct SchemeDimensions {
    double rectangleWidth = 0.0;
    double rectangleHeight = 0.0;
    double rootTileWidth = 1.0;
    double rootTileHeight = 1.0;
};

SchemeDimensions schemeDimensionsForRectangle(const TileScheme& scheme,
                                              const Rectangle& bounds) {
    SchemeDimensions dimensions;
    dimensions.rectangleWidth = std::max(1e-12, std::abs(bounds.width()));
    if (isWebMercatorScheme(scheme)) {
        dimensions.rectangleHeight = projectedHeight(scheme, bounds);
        dimensions.rootTileWidth = kTwoPi;
        dimensions.rootTileHeight = kTwoPi;
        if (scheme.id() == "OpenGlobus-Earth") {
            dimensions.rootTileHeight = kTwoPi / 3.0;
        }
        return dimensions;
    }
    dimensions.rectangleHeight = std::max(1e-12, std::abs(bounds.height()));
    if (scheme.id() == "Geographic-TMS") {
        dimensions.rootTileWidth = kPi;
        dimensions.rootTileHeight = kPi;
    } else {
        dimensions.rootTileWidth = kTwoPi;
        dimensions.rootTileHeight = kPi;
    }
    return dimensions;
}

int computeLevelFromTargetScreenPixels(const TileScheme& scheme,
                                       const ImageryProvider& provider,
                                       const Rectangle& bounds,
                                       double targetScreenPixelsX,
                                       double targetScreenPixelsY,
                                       double maximumScreenSpaceError,
                                       int minimumLevel,
                                       int maximumLevel) {
    const int minZoom =
        std::max({scheme.minZoom(), provider.minZoom(), minimumLevel});
    const int maxZoom =
        std::min({scheme.maxZoom(), provider.maxZoom(), maximumLevel});
    if (maxZoom < minZoom) return scheme.minZoom();

    const SchemeDimensions dimensions =
        schemeDimensionsForRectangle(scheme, bounds);
    const double rasterMaximumScreenSpaceError =
        std::max(1e-6, maximumScreenSpaceError);
    const double rasterPixelsX =
        std::max(1.0, targetScreenPixelsX) / rasterMaximumScreenSpaceError;
    const double rasterPixelsY =
        std::max(1.0, targetScreenPixelsY) / rasterMaximumScreenSpaceError;
    const double rasterTilesX =
        rasterPixelsX / static_cast<double>(std::max(1, provider.tileWidth()));
    const double rasterTilesY =
        rasterPixelsY / static_cast<double>(std::max(1, provider.tileHeight()));
    const double targetTileWidth =
        dimensions.rectangleWidth / std::max(1e-12, rasterTilesX);
    const double targetTileHeight =
        dimensions.rectangleHeight / std::max(1e-12, rasterTilesY);
    const double levelX = std::log2(
        dimensions.rootTileWidth / std::max(1e-12, targetTileWidth));
    const double levelY = std::log2(
        dimensions.rootTileHeight / std::max(1e-12, targetTileHeight));
    const int rounded = static_cast<int>(std::max(
        std::round(std::max(levelX, levelY)), 0.0));
    return std::clamp(rounded, minZoom, maxZoom);
}

bool matchesProviderRange(const ImageryProvider& provider,
                          const TileKey& key) {
    // Keep source-plan admission aligned with the existing quadtree contract:
    // a provider-specific supportsTile override must not suppress the desired
    // child request, because a failed/unsupported child can still resolve via
    // its source-parent fallback. Only the provider's scheme and level range
    // constrain enumeration here.
    return key.schemeId == provider.schemeId() &&
           key.z >= provider.minZoom() && key.z <= provider.maxZoom();
}

} // namespace

int RasterOverlaySourceRange::width() const {
    return std::max(0, maxX - minX + 1);
}

int RasterOverlaySourceRange::height() const {
    return std::max(0, maxY - minY + 1);
}

int RasterOverlaySourceRange::count() const {
    return width() * height();
}

int64_t RasterOverlaySourceRange::width64() const {
    return std::max<int64_t>(
        0,
        static_cast<int64_t>(maxX) - static_cast<int64_t>(minX) + 1);
}

int64_t RasterOverlaySourceRange::height64() const {
    return std::max<int64_t>(
        0,
        static_cast<int64_t>(maxY) - static_cast<int64_t>(minY) + 1);
}

int64_t RasterOverlaySourceRange::count64() const {
    const int64_t w = width64();
    const int64_t h = height64();
    if (w > 0 && h > std::numeric_limits<int64_t>::max() / w) {
        return std::numeric_limits<int64_t>::max();
    }
    return w * h;
}

int RasterOverlaySourceCoverage::width() const {
    int total = 0;
    for (const auto& range : ranges) total += range.width();
    return total;
}

int RasterOverlaySourceCoverage::height() const {
    int maximum = 0;
    for (const auto& range : ranges) maximum = std::max(maximum, range.height());
    return maximum;
}

int RasterOverlaySourceCoverage::count() const {
    int total = 0;
    for (const auto& range : ranges) total += range.count();
    return total;
}

int64_t RasterOverlaySourceCoverage::width64() const {
    int64_t total = 0;
    for (const auto& range : ranges) {
        const int64_t width = range.width64();
        if (total > std::numeric_limits<int64_t>::max() - width) {
            return std::numeric_limits<int64_t>::max();
        }
        total += width;
    }
    return total;
}

int64_t RasterOverlaySourceCoverage::height64() const {
    int64_t maximum = 0;
    for (const auto& range : ranges) maximum = std::max(maximum, range.height64());
    return maximum;
}

int64_t RasterOverlaySourceCoverage::count64() const {
    int64_t total = 0;
    for (const auto& range : ranges) {
        const int64_t count = range.count64();
        if (total > std::numeric_limits<int64_t>::max() - count) {
            return std::numeric_limits<int64_t>::max();
        }
        total += count;
    }
    return total;
}

RasterOverlaySourceRange RasterOverlaySourceCoverage::combinedRange() const {
    RasterOverlaySourceRange combined;
    if (ranges.empty()) return combined;
    combined = ranges.front();
    for (size_t i = 1; i < ranges.size(); ++i) {
        combined.minX = std::min(combined.minX, ranges[i].minX);
        combined.minY = std::min(combined.minY, ranges[i].minY);
        combined.maxX = std::max(combined.maxX, ranges[i].maxX);
        combined.maxY = std::max(combined.maxY, ranges[i].maxY);
    }
    return combined;
}

RasterOverlaySourceCoverage enumerateRasterOverlaySourceCoverage(
    const TileScheme& scheme,
    const Rectangle& bounds,
    int zoom) {
    RasterOverlaySourceCoverage coverage;
    const auto split = bounds.splitAtAntimeridian();
    coverage.ranges.push_back(trimBoundarySlop(
        scheme,
        split.first,
        zoom,
        computeRange(scheme, split.first, zoom)));
    if (split.second) {
        coverage.ranges.push_back(trimBoundarySlop(
            scheme,
            *split.second,
            zoom,
            computeRange(scheme, *split.second, zoom)));
    }
    return coverage;
}

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
    RasterOverlaySourceCoverage* selectedCoverage) {
    const int minZoom =
        std::max({scheme.minZoom(), provider.minZoom(), minimumLevel});
    const int maxZoom =
        std::min({scheme.maxZoom(), provider.maxZoom(), maximumLevel});
    if (maxZoom < minZoom) {
        if (selectedCoverage) *selectedCoverage = {};
        return scheme.minZoom();
    }

    int zoom = computeLevelFromTargetScreenPixels(
        scheme,
        provider,
        geometryBounds,
        targetScreenPixelsX,
        targetScreenPixelsY,
        maximumScreenSpaceError,
        minZoom,
        maxZoom);
    // CPU-local generated overlays (e.g. vector surface fill) declare a target
    // source zoom so the page subdivides with the camera display zoom, not with
    // the coarse terrain mesh zoom.  The maxTextureSize loop below still bounds
    // the composite page, so an unbounded value degrades gracefully.
    const int targetSourceZoom = provider.targetSourceZoom();
    if (targetSourceZoom >= 0) {
        zoom = std::clamp(targetSourceZoom, minZoom, maxZoom);
    }
    const int maxTextureSize = maximumTextureSize > 0
        ? maximumTextureSize
        : kMaximumTextureSizeFallback;
    RasterOverlaySourceCoverage coverage =
        enumerateRasterOverlaySourceCoverage(scheme, sourceBounds, zoom);
    while (zoom > minZoom) {
        const int64_t widthPixels = saturatingPixelSpan(
            coverage.width64(), std::max(1, provider.tileWidth()));
        const int64_t heightPixels = saturatingPixelSpan(
            coverage.height64(), std::max(1, provider.tileHeight()));
        if (widthPixels <= maxTextureSize && heightPixels <= maxTextureSize) {
            break;
        }
        --zoom;
        coverage = enumerateRasterOverlaySourceCoverage(
            scheme, sourceBounds, zoom);
    }
    if (selectedCoverage) *selectedCoverage = std::move(coverage);
    return zoom;
}

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
    int maximumLevel) {
    RasterOverlaySourcePlan plan;
    RasterOverlaySourceCoverage coverage;
    plan.sourceZoom = chooseRasterOverlaySourceZoom(
        scheme,
        provider,
        geometryBounds,
        sourceBounds,
        targetScreenPixelsX,
        targetScreenPixelsY,
        maximumScreenSpaceError,
        maximumTextureSize,
        minimumLevel,
        maximumLevel,
        &coverage);
    const RasterOverlaySourceRange range = coverage.combinedRange();
    plan.sourceBounds = sourceBounds;
    plan.minX = range.minX;
    plan.minY = range.minY;
    plan.maxX = range.maxX;
    plan.maxY = range.maxY;
    plan.sourceKeys.reserve(static_cast<size_t>(std::min<int64_t>(
        coverage.count64(), kMaximumSourcePlanReserve)));

    for (const auto& coveredRange : coverage.ranges) {
        for (int x = coveredRange.minX; x <= coveredRange.maxX; ++x) {
            for (int y = coveredRange.minY; y <= coveredRange.maxY; ++y) {
                TileKey sourceKey{scheme.id(), plan.sourceZoom, x, y};
                if (matchesProviderRange(provider, sourceKey) &&
                    rectanglesOverlapWithArea(
                        scheme.tileToRectangle(sourceKey), sourceBounds)) {
                    plan.sourceKeys.push_back(sourceKey);
                }
            }
        }
    }

    // Preserve the existing one-tile expansion for numerical edge cases where
    // the trimmed range contains coverage but no provider-supported key.
    if (plan.sourceKeys.empty() && coverage.count() > 0) {
        const int maxTileX = scheme.tileCountX(plan.sourceZoom) - 1;
        const int maxTileY = scheme.tileCountY(plan.sourceZoom) - 1;
        const int minX = std::max(0, range.minX - 1);
        const int maxX = std::min(maxTileX, range.maxX + 1);
        const int minY = std::max(0, range.minY - 1);
        const int maxY = std::min(maxTileY, range.maxY + 1);
        for (int x = minX; x <= maxX; ++x) {
            for (int y = minY; y <= maxY; ++y) {
                TileKey sourceKey{scheme.id(), plan.sourceZoom, x, y};
                if (matchesProviderRange(provider, sourceKey) &&
                    rectanglesOverlapWithArea(
                        scheme.tileToRectangle(sourceKey), sourceBounds)) {
                    plan.sourceKeys.push_back(sourceKey);
                }
            }
        }
        if (!plan.sourceKeys.empty()) {
            plan.minX = minX;
            plan.minY = minY;
            plan.maxX = maxX;
            plan.maxY = maxY;
        }
    }
    if (plan.sourceKeys.size() == 1) {
        const Rectangle sourceTileBounds =
            scheme.tileToRectangle(plan.sourceKeys.front());
        plan.exactSingleSource = rectanglesEqualForDirectRasterTile(
                                     geometryBounds, sourceTileBounds) &&
                                 rectanglesEqualForDirectRasterTile(
                                     sourceBounds, sourceTileBounds);
    }
    return plan;
}

} // namespace earth_engine
