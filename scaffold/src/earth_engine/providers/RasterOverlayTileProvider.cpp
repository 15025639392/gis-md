#include "RasterOverlayTileProvider.h"
#include "ImageryProvider.h"
#include "../layers/RasterOverlay.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../tiling/TileScheme.h"
#include "RasterTextureUploader.h"
#include "../renderer/RenderDevice.h"
#include "../threading/CancellationToken.h"
#include "../core/async/AsyncSystem.h"
#include "../debug/PerfTimer.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Projection.h"
#include "../core/math/MathUtils.h"
#include "../debug/PlatformLog.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {
namespace {

constexpr uint64_t kRetainedUnusedFrames = 120;
constexpr int kMaximumCombinedTextureSizeFallback = 2048;
constexpr size_t kDefaultMaximumRasterUploadsPerFrame = 20;
constexpr int64_t kMaximumSourcePlanReserve = 1'000'000;
constexpr int kInteractionRasterUploadMaxDimension = 512;
constexpr int64_t kInteractionRasterUploadMaxPixels = 512ll * 512ll;
constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kMaxWebMercatorLat = 1.4844222297453324;
constexpr double kPixelTolerance = 0.01;

double webMercatorY(double latRad);

void logAndroidRasterPipeline(const char* stage,
                              const std::string& cacheKey,
                              int sourceCount,
                              int sourceZoom) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) >= 48) {
        return;
    }
    platformLog(
        LogLevel::Info,
        "RasterOverlayPipe",
        "%s cache=%s sources=%d sourceZoom=%d",
        stage,
        cacheKey.c_str(),
        sourceCount,
        sourceZoom);
}

void decrementActiveRasterTileLoads(std::atomic<uint32_t>& activeLoads) {
    uint32_t current = activeLoads.load(
        std::memory_order_relaxed);
    while (current > 0 &&
           !activeLoads.compare_exchange_weak(
               current,
               current - 1,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

/// 节流名额唯一释放：完成回调与 abandon/析构可能并发认领同一名额
/// （completed 置位到回调 erase 之间条目仍在 activeMappedSourceSets），
/// 以 exchange 决定唯一递减方，防止双重释放静默偷走其他在途名额。
void releaseRasterThrottleSlotOnce(std::atomic<bool>& released,
                                   std::atomic<uint32_t>& activeLoads) {
    if (!released.exchange(true)) {
        decrementActiveRasterTileLoads(activeLoads);
    }
}

bool uploadAllowedDuringInteraction(
    const std::string& cacheKey,
    const DecodedImage* image) {
    // 交互期只按单次上传成本（尺寸）过滤；节奏由 budget 的
    // RasterTextureUpload lane 控制（TileFrameResourceBudgetPlanner 在
    // smoothing/交互下给出时间基或 ≤8/帧 的涓流额度）。此前对
    // "mapped-raster/" 前缀无条件排除：长交互把影像上传全量积压
    // （真机 60+ pendUp），交互期影像完全停更。≤512² 的 mapped raster
    // 单次上传 <1ms，交由 lane 限额涓流即可。
    (void)cacheKey;
    if (!image) {
        return true;
    }
    if (image->width > kInteractionRasterUploadMaxDimension ||
        image->height > kInteractionRasterUploadMaxDimension) {
        return false;
    }
    const int64_t pixels = static_cast<int64_t>(image->width) *
                           static_cast<int64_t>(image->height);
    return pixels <= kInteractionRasterUploadMaxPixels;
}

Projection projectionVariant(RasterOverlayProjection projection) {
    switch (projection) {
        case RasterOverlayProjection::Geographic:
            return GeographicProjection(Ellipsoid::WGS84());
        case RasterOverlayProjection::WebMercator:
            return WebMercatorProjection(Ellipsoid::WGS84());
    }
    return GeographicProjection(Ellipsoid::WGS84());
}

RasterOverlayProjection projectionForScheme(const TileScheme& scheme) {
    return scheme.crsProfile() == "EPSG:3857"
        ? RasterOverlayProjection::WebMercator
        : RasterOverlayProjection::Geographic;
}

Rectangle projectGeographicToProvider(const Rectangle& rectangle,
                                      RasterOverlayProjection projection) {
    if (projection == RasterOverlayProjection::Geographic) {
        return rectangle;
    }
    return projectRectangleSimple(projectionVariant(projection), rectangle);
}

Rectangle unprojectProviderToGeographic(const Rectangle& rectangle,
                                        RasterOverlayProjection projection) {
    if (projection == RasterOverlayProjection::Geographic) {
        return rectangle;
    }
    return unprojectRectangleSimple(projectionVariant(projection), rectangle);
}

int maximumCombinedTextureSize(const RasterTextureUploader* uploader,
                               int configuredMaximumTextureSize) {
    const int configuredMaxTextureSize =
        configuredMaximumTextureSize > 0
            ? configuredMaximumTextureSize
            : kMaximumCombinedTextureSizeFallback;
    if (!uploader) return configuredMaxTextureSize;
    const int backendMaxTextureSize = uploader->maxTextureSize();
    if (backendMaxTextureSize <= 0) {
        return configuredMaxTextureSize;
    }
    return std::max(1, std::min(backendMaxTextureSize,
                                configuredMaxTextureSize));
}

struct TileRange {
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;

    int width() const { return std::max(0, maxX - minX + 1); }
    int height() const { return std::max(0, maxY - minY + 1); }
    int count() const { return width() * height(); }
    int64_t width64() const {
        return std::max<int64_t>(
            0,
            static_cast<int64_t>(maxX) - static_cast<int64_t>(minX) + 1);
    }
    int64_t height64() const {
        return std::max<int64_t>(
            0,
            static_cast<int64_t>(maxY) - static_cast<int64_t>(minY) + 1);
    }
    int64_t count64() const {
        const int64_t w = width64();
        const int64_t h = height64();
        if (w > 0 &&
            h > std::numeric_limits<int64_t>::max() / w) {
            return std::numeric_limits<int64_t>::max();
        }
        return w * h;
    }
};

struct TileCoverage {
    std::vector<TileRange> ranges;

    int width() const {
        int total = 0;
        for (const TileRange& range : ranges) {
            total += range.width();
        }
        return total;
    }

    int height() const {
        int maximum = 0;
        for (const TileRange& range : ranges) {
            maximum = std::max(maximum, range.height());
        }
        return maximum;
    }

    int count() const {
        int total = 0;
        for (const TileRange& range : ranges) {
            total += range.count();
        }
        return total;
    }

    int64_t width64() const {
        int64_t total = 0;
        for (const TileRange& range : ranges) {
            const int64_t width = range.width64();
            if (total > std::numeric_limits<int64_t>::max() - width) {
                return std::numeric_limits<int64_t>::max();
            }
            total += width;
        }
        return total;
    }

    int64_t height64() const {
        int64_t maximum = 0;
        for (const TileRange& range : ranges) {
            maximum = std::max(maximum, range.height64());
        }
        return maximum;
    }

    int64_t count64() const {
        int64_t total = 0;
        for (const TileRange& range : ranges) {
            const int64_t count = range.count64();
            if (total > std::numeric_limits<int64_t>::max() - count) {
                return std::numeric_limits<int64_t>::max();
            }
            total += count;
        }
        return total;
    }

    TileRange combinedRange() const {
        TileRange combined;
        if (ranges.empty()) {
            return combined;
        }
        combined = ranges.front();
        for (size_t i = 1; i < ranges.size(); ++i) {
            combined.minX = std::min(combined.minX, ranges[i].minX);
            combined.minY = std::min(combined.minY, ranges[i].minY);
            combined.maxX = std::max(combined.maxX, ranges[i].maxX);
            combined.maxY = std::max(combined.maxY, ranges[i].maxY);
        }
        return combined;
    }
};

TileRange computeRange(const TileScheme& scheme,
                       const Rectangle& bounds,
                       int zoom) {
    TileRange range;
    scheme.tileRange(bounds, zoom, range.minX, range.minY, range.maxX, range.maxY);
    if (range.maxX < range.minX) std::swap(range.maxX, range.minX);
    if (range.maxY < range.minY) std::swap(range.maxY, range.minY);
    return range;
}

TileRange trimCesiumNativeBoundarySlop(const TileScheme& scheme,
                                       const Rectangle& geometryBounds,
                                       int zoom,
                                       TileRange range) {
    if (range.maxX < range.minX || range.maxY < range.minY) {
        return range;
    }

    // cesium-native QuadtreeRasterOverlayTileProvider excludes tiles that only
    // touch a geometry rectangle along a tile edge, using 1/512 of the geometry
    // span as the edge tolerance.
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

TileCoverage computeCoverage(const TileScheme& scheme,
                             const Rectangle& bounds,
                             int zoom) {
    TileCoverage coverage;
    const auto split = bounds.splitAtAntimeridian();
    coverage.ranges.push_back(trimCesiumNativeBoundarySlop(
        scheme,
        split.first,
        zoom,
        computeRange(scheme, split.first, zoom)));
    if (split.second) {
        coverage.ranges.push_back(trimCesiumNativeBoundarySlop(
            scheme,
            *split.second,
            zoom,
            computeRange(scheme, *split.second, zoom)));
    }
    return coverage;
}

int availableRasterRequestSlots(FrameResourceBudget* budget,
                                uint32_t currentInflight) {
    if (!budget) {
        return std::numeric_limits<int>::max();
    }
    const FrameResourceBudgetSnapshot snapshot = budget->snapshot();
    if (currentInflight >= snapshot.maxRasterNetworkInflight ||
        snapshot.rasterNetworkRequestsIssued >=
            snapshot.maxRasterNetworkRequestsPerFrame) {
        return 0;
    }
    const uint32_t frameSlots =
        snapshot.maxRasterNetworkRequestsPerFrame -
        snapshot.rasterNetworkRequestsIssued;
    const uint32_t inflightSlots =
        snapshot.maxRasterNetworkInflight - currentInflight;
    return static_cast<int>(std::min(frameSlots, inflightSlots));
}

bool hasRasterInflightCapacity(FrameResourceBudget* budget,
                               uint32_t currentInflight,
                               int estimatedFanout) {
    if (!budget) {
        return true;
    }
    return budget->hasNetworkInflightCapacity(
        FrameResourceLane::RasterRequest,
        currentInflight,
        estimatedFanout);
}

int64_t saturatingPixelSpan(int64_t tiles, int tilePixels) {
    if (tiles <= 0 || tilePixels <= 0) {
        return 0;
    }
    const int64_t pixels = static_cast<int64_t>(tilePixels);
    if (tiles > std::numeric_limits<int64_t>::max() / pixels) {
        return std::numeric_limits<int64_t>::max();
    }
    return tiles * pixels;
}

bool isWebMercatorScheme(const TileScheme& scheme) {
    const std::string id = scheme.id();
    return id == "XYZ-WebMercator" ||
           id == "TMS-WebMercator" ||
           id == "OpenGlobus-Earth";
}

bool rectanglesOverlapWithArea(const Rectangle& a, const Rectangle& b) {
    std::optional<Rectangle> intersection = a.computeIntersection(b);
    return intersection &&
           intersection->width() > 1e-15 &&
           intersection->height() > 1e-15;
}

double inwardSampleEpsilon(double span) {
    return std::max(1e-12, std::abs(span) * 1e-9);
}

Rectangle expandClampedLineIntoCoverage(const Rectangle& bounds,
                                        const Rectangle& coverage) {
    double west = bounds.west();
    double east = bounds.east();
    double south = bounds.south();
    double north = bounds.north();

    if (east <= west) {
        const double epsilon =
            std::min(inwardSampleEpsilon(coverage.width()),
                     std::max(1e-12, coverage.width()));
        if (west >= coverage.east()) {
            west = coverage.east() - epsilon;
            east = coverage.east();
        } else if (east <= coverage.west()) {
            west = coverage.west();
            east = coverage.west() + epsilon;
        } else {
            west = std::max(coverage.west(), west - epsilon * 0.5);
            east = std::min(coverage.east(), east + epsilon * 0.5);
            if (east <= west) {
                east = std::min(coverage.east(), west + epsilon);
            }
        }
    }

    if (north <= south) {
        const double epsilon =
            std::min(inwardSampleEpsilon(coverage.height()),
                     std::max(1e-12, coverage.height()));
        if (south >= coverage.north()) {
            south = coverage.north() - epsilon;
            north = coverage.north();
        } else if (north <= coverage.south()) {
            south = coverage.south();
            north = coverage.south() + epsilon;
        } else {
            south = std::max(coverage.south(), south - epsilon * 0.5);
            north = std::min(coverage.north(), north + epsilon * 0.5);
            if (north <= south) {
                north = std::min(coverage.north(), south + epsilon);
            }
        }
    }

    return Rectangle(west, south, east, north);
}

std::optional<Rectangle> mapGeometryBoundsToImageryCoverage(
    const Rectangle& geometryBounds,
    const Rectangle& coverage,
    bool clampOutsideCoverage) {
    if (coverage.contains(geometryBounds)) {
        return geometryBounds;
    }

    std::optional<Rectangle> intersection =
        geometryBounds.computeIntersection(coverage);
    if (intersection) {
        return *intersection;
    }

    if (!clampOutsideCoverage) {
        return std::nullopt;
    }

    // cesium-native QuadtreeRasterOverlayTileProvider currently maps every
    // overlay with no geometry/provider overlap to the nearest coverage edge.
    // The source still contains a TODO for base-layer-only behavior, but the
    // implemented behavior stretches edge texels for all overlay roles.
    double west = 0.0;
    double east = 0.0;
    if (geometryBounds.west() >= coverage.east()) {
        west = east = coverage.east();
    } else if (geometryBounds.east() <= coverage.west()) {
        west = east = coverage.west();
    } else {
        west = std::max(geometryBounds.west(), coverage.west());
        east = std::min(geometryBounds.east(), coverage.east());
    }

    double south = 0.0;
    double north = 0.0;
    if (geometryBounds.south() >= coverage.north()) {
        south = north = coverage.north();
    } else if (geometryBounds.north() <= coverage.south()) {
        south = north = coverage.south();
    } else {
        south = std::max(geometryBounds.south(), coverage.south());
        north = std::min(geometryBounds.north(), coverage.north());
    }

    return expandClampedLineIntoCoverage(
        Rectangle(west, south, east, north),
        coverage);
}

bool shouldClampOutsideCoverage(const RasterOverlay* owner) {
    (void)owner;
    return true;
}

Rectangle schemeCoverageRectangle(const TileScheme& scheme) {
    const int rootX = std::max(1, scheme.tileCountX(0));
    const int rootY = std::max(1, scheme.tileCountY(0));
    Rectangle result = scheme.tileToRectangle(TileKey{scheme.id(), 0, 0, 0});
    for (int y = 0; y < rootY; ++y) {
        for (int x = 0; x < rootX; ++x) {
            if (x == 0 && y == 0) {
                continue;
            }
            result = result.computeUnion(
                scheme.tileToRectangle(TileKey{scheme.id(), 0, x, y}));
        }
    }
    return result;
}

Rectangle effectiveCoverageRectangle(
    const TileScheme& scheme,
    const Rectangle& providerCoverage) {
    const Rectangle schemeRectangle = schemeCoverageRectangle(scheme);
    return schemeRectangle.computeIntersection(providerCoverage)
        .value_or(schemeRectangle);
}

bool isDecodedImageUploadable(const DecodedImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.channels <= 0 ||
        image.bytesPerChannel <= 0) {
        return false;
    }
    const int64_t requiredBytes =
        static_cast<int64_t>(image.width) *
        static_cast<int64_t>(image.height) *
        static_cast<int64_t>(image.channels) *
        static_cast<int64_t>(image.bytesPerChannel);
    return requiredBytes > 0 &&
           image.pixels.size() >= static_cast<size_t>(requiredBytes);
}

bool isRasterCompositeSourceImage(const DecodedImage& image) {
    return isDecodedImageUploadable(image) &&
           (image.channels == 1 || image.channels == 3 ||
            image.channels == 4);
}

int64_t decodedImageSizeBytes(const DecodedImage& image) {
    // Provider-side cache / backpressure budgets should track raster pixel
    // payload, not the C++ wrapper object. Empty "loaded without texture"
    // placeholders must not evict real cached source imagery.
    return static_cast<int64_t>(image.pixels.size());
}

void trackPeakBytes(int64_t currentBytes, int64_t& peakBytes) {
    if (currentBytes > peakBytes) {
        peakBytes = currentBytes;
    }
}

double webMercatorY(double latRad) {
    const double lat = std::clamp(
        latRad, -kMaxWebMercatorLat, kMaxWebMercatorLat);
    return std::log(std::tan(lat * 0.5 + kPi * 0.25));
}

double latitudeAtProjectedY(const TileScheme& scheme, double projectedY) {
    const double latitude = isWebMercatorScheme(scheme)
        ? std::atan(std::sinh(projectedY))
        : projectedY;
    return std::abs(latitude) < 1e-15 ? 0.0 : latitude;
}

double projectedSouth(const TileScheme& scheme, const Rectangle& bounds) {
    return isWebMercatorScheme(scheme)
        ? webMercatorY(bounds.south())
        : bounds.south();
}

double projectedNorth(const TileScheme& scheme, const Rectangle& bounds) {
    return isWebMercatorScheme(scheme)
        ? webMercatorY(bounds.north())
        : bounds.north();
}

double projectedHeight(const TileScheme& scheme, const Rectangle& bounds) {
    return std::max(
        1e-12,
        std::abs(projectedNorth(scheme, bounds) -
                 projectedSouth(scheme, bounds)));
}

double projectedVForLatitudeInternal(const TileScheme& scheme,
                                     const Rectangle& bounds,
                                     double lat) {
    const double north = projectedNorth(scheme, bounds);
    const double south = projectedSouth(scheme, bounds);
    const double h = std::max(1e-12, std::abs(north - south));
    const double projected = isWebMercatorScheme(scheme)
        ? webMercatorY(lat)
        : lat;
    return std::clamp((north - projected) / h, 0.0, 1.0);
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
        std::max(1.0, targetScreenPixelsX) /
        rasterMaximumScreenSpaceError;
    const double rasterPixelsY =
        std::max(1.0, targetScreenPixelsY) /
        rasterMaximumScreenSpaceError;
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

int chooseQuadtreeSourceZoom(const TileScheme& scheme,
                        const ImageryProvider& provider,
                        const RasterTextureUploader* uploader,
                        const Rectangle& geometryBounds,
                        const Rectangle& sourceBounds,
                        double targetScreenPixelsX,
                        double targetScreenPixelsY,
                        double maximumScreenSpaceError,
                        int maximumTextureSize,
                        int minimumLevel,
                        int maximumLevel,
                        TileRange* outRange = nullptr) {
    const int minZoom =
        std::max({scheme.minZoom(), provider.minZoom(), minimumLevel});
    const int maxZoom =
        std::min({scheme.maxZoom(), provider.maxZoom(), maximumLevel});
    if (maxZoom < minZoom) {
        if (outRange) *outRange = TileRange{};
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
    const int maxTextureSize =
        maximumCombinedTextureSize(uploader, maximumTextureSize);

    TileCoverage coverage = computeCoverage(scheme, sourceBounds, zoom);
    while (zoom > minZoom) {
        const int64_t widthPixels =
            saturatingPixelSpan(
                coverage.width64(),
                std::max(1, provider.tileWidth()));
        const int64_t heightPixels =
            saturatingPixelSpan(
                coverage.height64(),
                std::max(1, provider.tileHeight()));
        if (widthPixels <= maxTextureSize &&
            heightPixels <= maxTextureSize) {
            break;
        }
        --zoom;
        coverage = computeCoverage(scheme, sourceBounds, zoom);
    }

    if (outRange) *outRange = coverage.combinedRange();
    return zoom;
}

double normalizeRectangleCacheCoordinate(double value) {
    return std::abs(value) < 1e-15 ? 0.0 : value;
}

std::string rectangleCacheKey(const Rectangle& rectangle) {
    char bounds[256];
    std::snprintf(bounds,
                  sizeof(bounds),
                  "%.15g/%.15g/%.15g/%.15g",
                  normalizeRectangleCacheCoordinate(rectangle.west()),
                  normalizeRectangleCacheCoordinate(rectangle.south()),
                  normalizeRectangleCacheCoordinate(rectangle.east()),
                  normalizeRectangleCacheCoordinate(rectangle.north()));
    return bounds;
}

std::string mappedRasterTileCacheKey(
    const TileScheme& scheme,
    const Rectangle& geometryRectangle,
    const Rectangle& sourceBounds,
    const RasterOverlayTileProvider::RasterSourceTileMapping& sourceTiles,
    uint64_t epoch) {
    std::string key = "mapped-raster/epoch/" + std::to_string(epoch) + "/" +
                      scheme.id() + "/srcz/" +
                      std::to_string(sourceTiles.sourceZoom) + "/geom/" +
                      rectangleCacheKey(geometryRectangle) + "/src/" +
                      rectangleCacheKey(sourceBounds) + "/range/" +
                      std::to_string(sourceTiles.minX) + "/" +
                      std::to_string(sourceTiles.minY) + "/" +
                      std::to_string(sourceTiles.maxX) + "/" +
                      std::to_string(sourceTiles.maxY) + "/tiles";
    for (const TileKey& sourceKey : sourceTiles.sourceKeys) {
        key += "/" + std::to_string(sourceKey.z) + "/" +
               std::to_string(sourceKey.x) + "/" +
               std::to_string(sourceKey.y);
    }
    return key;
}

bool rectanglesEqualForDirectRasterTile(const Rectangle& a,
                                        const Rectangle& b) {
    const double span =
        std::max({std::abs(a.width()),
                  std::abs(a.height()),
                  std::abs(b.width()),
                  std::abs(b.height()),
                  1.0});
    return a.equalsEpsilon(b, span * 1e-12);
}

bool matchesProviderQuadtreeRange(const ImageryProvider& provider,
                                  const TileKey& key) {
    return key.schemeId == provider.schemeId() &&
           key.z >= provider.minZoom() &&
           key.z <= provider.maxZoom();
}

TileKey parentTileKey(const TileKey& key) {
    return key.parent();
}

std::string sourceCacheKey(const TileKey& key) {
    return key.schemeId.str() + "/" + std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" + std::to_string(key.y);
}

std::string sourceCacheKey(uint64_t epoch, const TileKey& key) {
    return "epoch/" + std::to_string(epoch) + "/" + sourceCacheKey(key);
}

std::unique_ptr<TileScheme> createAsyncSchemeSnapshot(
    const TileScheme& scheme) {
    const std::string id = scheme.id();
    if (id == "XYZ-WebMercator") {
        return TileScheme::createXYZWebMercator();
    }
    if (id == "TMS-WebMercator") {
        return TileScheme::createTMS();
    }
    if (id == "OpenGlobus-Earth") {
        return TileScheme::createOpenGlobusEarth();
    }
    if (id == "Geographic-TMS") {
        return TileScheme::createGeographicTMS();
    }
    return nullptr;
}

struct RasterSourceResult {
    TileKey key;
    Rectangle bounds;
    std::shared_ptr<const DecodedImage> image;
    std::optional<Rectangle> sourceSubset;
    RasterOverlayTile::MoreDetailAvailable moreDetailAvailable =
        RasterOverlayTile::MoreDetailAvailable::Unknown;
    std::vector<std::string> diagnostics;
    std::vector<std::string> credits;
    bool terminalFailure = false;
};

bool isResolvedRasterSourceResult(const RasterSourceResult& source) {
    return source.image || source.terminalFailure;
}

bool hasNonAncestorRasterSourceImage(
    const std::vector<RasterSourceResult>& sources) {
    return std::any_of(
        sources.begin(),
        sources.end(),
        [](const RasterSourceResult& source) {
            return source.image && !source.sourceSubset.has_value();
        });
}

void appendCredits(std::vector<std::string>& target,
                   const std::vector<std::string>& credits) {
    for (const std::string& credit : credits) {
        if (credit.empty()) {
            continue;
        }
        target.push_back(credit);
    }
}

struct PixelRectangle {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct CombinedImageMeasurements {
    Rectangle rectangle;
    int width = 0;
    int height = 0;
    int channels = 0;
    int bytesPerChannel = 1;
};

/// Projected-equivalent bounds for a raster source, used by
/// measureCombinedImage after the pipeline entry projection.
struct ProjectedSource {
    Rectangle bounds;
    std::optional<Rectangle> sourceSubset;
};

PixelRectangle computePixelRectangle(
    const DecodedImage& image,
    const Rectangle& totalRectangle,
    const Rectangle& partRectangle) {

    int x = static_cast<int>(MathUtils::roundDown(
        image.width * (partRectangle.west() - totalRectangle.west()) /
            totalRectangle.width(),
        kPixelTolerance));
    x = std::max(0, x);

    int y = static_cast<int>(MathUtils::roundDown(
        image.height * (totalRectangle.north() - partRectangle.north()) /
            totalRectangle.height(),
        kPixelTolerance));
    y = std::max(0, y);

    int maxX = static_cast<int>(MathUtils::roundUp(
        image.width * (partRectangle.east() - totalRectangle.west()) /
            totalRectangle.width(),
        kPixelTolerance));
    maxX = std::min(maxX, image.width);

    int maxY = static_cast<int>(MathUtils::roundUp(
        image.height * (totalRectangle.north() - partRectangle.south()) /
            totalRectangle.height(),
        kPixelTolerance));
    maxY = std::min(maxY, image.height);

    return PixelRectangle{x, y, std::max(0, maxX - x), std::max(0, maxY - y)};
}

CombinedImageMeasurements measureCombinedImage(
    const Rectangle& targetBounds,
    const std::vector<ProjectedSource>& projectedSources,
    const std::vector<RasterSourceResult>& sources,
    double projectedWidthPerPixel,
    double projectedHeightPerPixel) {
    std::optional<Rectangle> combinedBounds;
    int channels = 0;
    int bytesPerChannel = 1;
    for (size_t i = 0; i < sources.size(); ++i) {
        const RasterSourceResult& source = sources[i];
        if (source.image) {
            channels = std::max(channels, source.image->channels);
            bytesPerChannel =
                std::max(bytesPerChannel, source.image->bytesPerChannel);
        }
        const Rectangle sourceRect = i < projectedSources.size()
            ? projectedSources[i].sourceSubset.value_or(projectedSources[i].bounds)
            : source.bounds;
        // Projection-space intersection — compute directly without
        // antimeridian wrapping (computeIntersection adds kTwoPi for geographic
        // coords, which corrupts projected meters).
        double projWest = std::max(targetBounds.west(), sourceRect.west());
        double projEast = std::min(targetBounds.east(), sourceRect.east());
        double projSouth = std::max(targetBounds.south(), sourceRect.south());
        double projNorth = std::min(targetBounds.north(), sourceRect.north());
        if (projWest >= projEast || projSouth >= projNorth) {
            continue; // no overlap in projection space
        }
        Rectangle intersection(projWest, projSouth, projEast, projNorth);

        const double roundedWest =
            MathUtils::roundDown(
                intersection.west() / projectedWidthPerPixel,
                kPixelTolerance) *
            projectedWidthPerPixel;
        const double roundedSouth =
            MathUtils::roundDown(
                intersection.south() / projectedHeightPerPixel,
                kPixelTolerance) *
            projectedHeightPerPixel;
        const double roundedEast =
            MathUtils::roundUp(
                intersection.east() / projectedWidthPerPixel,
                kPixelTolerance) *
            projectedWidthPerPixel;
        const double roundedNorth =
            MathUtils::roundUp(
                intersection.north() / projectedHeightPerPixel,
                kPixelTolerance) *
            projectedHeightPerPixel;

        if (roundedWest > roundedEast || roundedSouth > roundedNorth) {
            continue; // degenerate after rounding — skip
        }
        Rectangle expanded(roundedWest, roundedSouth, roundedEast, roundedNorth);

        if (expanded.west() == expanded.east()) {
            expanded = Rectangle(
                expanded.west(),
                expanded.south(),
                expanded.east() + projectedWidthPerPixel,
                expanded.north());
        }
        if (expanded.south() == expanded.north()) {
            expanded = Rectangle(
                expanded.west(),
                expanded.south(),
                expanded.east(),
                expanded.north() + projectedHeightPerPixel);
        }

        // Manual projected-space union — computeUnion calls
        // convertLongitudeRange which corrupts WebMercator meter values
        // (mods ~20M by 2π, turning 20M into ~0).
        // Use direct min/max instead, matching cesium-native behavior
        // where expanded rectangles are in projection space.
        if (!combinedBounds) {
            combinedBounds = expanded;
        } else {
            combinedBounds = Rectangle(
                std::min(combinedBounds->west(), expanded.west()),
                std::min(combinedBounds->south(), expanded.south()),
                std::max(combinedBounds->east(), expanded.east()),
                std::max(combinedBounds->north(), expanded.north()));
        }
    }

    if (!combinedBounds) {
        return {};
    }

    int width = static_cast<int>(MathUtils::roundUp(
        combinedBounds->computeWidth() / projectedWidthPerPixel,
        kPixelTolerance));
    int height = static_cast<int>(MathUtils::roundUp(
        combinedBounds->computeHeight() / projectedHeightPerPixel,
        kPixelTolerance));
    width = std::max(1, width);
    height = std::max(1, height);
    return CombinedImageMeasurements{
        *combinedBounds,
        width,
        height,
        channels,
        bytesPerChannel};
}

namespace {

/// Unsafe row-wise memory copy with support for different source/target row
/// strides and channel counts. Matches cesium-native
/// ImageManipulation::unsafeBlitImage. When source has fewer channels than
/// target, the missing channels (alpha) are set to 0xFF.
void unsafeBlitImage(uint8_t* pTarget,
                     size_t targetRowStride,
                     size_t targetChannels,
                     int targetBytesPerChannel,
                     const uint8_t* pSource,
                     size_t sourceRowStride,
                     size_t sourceChannels,
                     int sourceBytesPerChannel,
                     size_t sourceWidth,
                     size_t sourceHeight,
                     size_t bytesPerPixel) {
    if (sourceChannels == targetChannels &&
        sourceBytesPerChannel == targetBytesPerChannel) {
        const size_t bytesToCopyPerRow = bytesPerPixel * sourceWidth;
        if (bytesToCopyPerRow == targetRowStride &&
            targetRowStride == sourceRowStride) {
            std::memcpy(pTarget, pSource,
                        sourceWidth * sourceHeight * bytesPerPixel);
        } else {
            for (size_t j = 0; j < sourceHeight; ++j) {
                std::memcpy(pTarget, pSource, bytesToCopyPerRow);
                pTarget += targetRowStride;
                pSource += sourceRowStride;
            }
        }
    } else {
        // Channel or bytesPerChannel mismatch: copy per-pixel/chan.
        // For narrower source channels/bytes, the target output is cleared
        // (high bytes zeroed, missing channels set to 0xFF).
        const size_t sourceBytesPerChan = static_cast<size_t>(sourceBytesPerChannel);
        const size_t targetBytesPerChan = static_cast<size_t>(targetBytesPerChannel);
        const size_t sourceBytesPerPixel = sourceChannels * sourceBytesPerChan;
        for (size_t j = 0; j < sourceHeight; ++j) {
            for (size_t i = 0; i < sourceWidth; ++i) {
                size_t c = 0;
                for (; c < sourceChannels; ++c) {
                    size_t cOff = i * bytesPerPixel + c * targetBytesPerChan;
                    for (size_t b = 0; b < sourceBytesPerChan; ++b) {
                        pTarget[cOff + b] =
                            pSource[i * sourceBytesPerPixel +
                                    c * sourceBytesPerChan + b];
                    }
                    for (size_t b = sourceBytesPerChan; b < targetBytesPerChan; ++b) {
                        pTarget[cOff + b] = 0;
                    }
                }
                for (; c < targetChannels; ++c) {
                    std::memset(pTarget + i * bytesPerPixel + c * targetBytesPerChan,
                                0xFF, targetBytesPerChan);
                }
            }
            pTarget += targetRowStride;
            pSource += sourceRowStride;
        }
    }
}

/// Bilinear interpolation for 1-byte-per-channel images (cesium-native pattern).
/// Used when source and destination rectangles differ in pixel dimensions.
void unsafeBilinearResize(uint8_t* pTarget,
                          int targetWidth,
                          int targetHeight,
                          size_t targetRowStride,
                          const uint8_t* pSource,
                          int sourceWidth,
                          int sourceHeight,
                          size_t sourceRowStride,
                          int channels) {
    for (int ty = 0; ty < targetHeight; ++ty) {
        const double sy_f = static_cast<double>(ty) *
            static_cast<double>(sourceHeight - 1) /
            static_cast<double>(std::max(targetHeight - 1, 1));
        const int sy0 = std::min(static_cast<int>(sy_f), sourceHeight - 2);
        const int sy1 = sy0 + 1;
        const double vy = sy_f - static_cast<double>(sy0);

        for (int tx = 0; tx < targetWidth; ++tx) {
            const double sx_f = static_cast<double>(tx) *
                static_cast<double>(sourceWidth - 1) /
                static_cast<double>(std::max(targetWidth - 1, 1));
            const int sx0 = std::min(static_cast<int>(sx_f), sourceWidth - 2);
            const int sx1 = sx0 + 1;
            const double vx = sx_f - static_cast<double>(sx0);

            for (int c = 0; c < channels; ++c) {
                const double p00 = pSource[static_cast<size_t>(sy0) * sourceRowStride +
                                           static_cast<size_t>(sx0) * static_cast<size_t>(channels) +
                                           static_cast<size_t>(c)];
                const double p10 = pSource[static_cast<size_t>(sy0) * sourceRowStride +
                                           static_cast<size_t>(sx1) * static_cast<size_t>(channels) +
                                           static_cast<size_t>(c)];
                const double p01 = pSource[static_cast<size_t>(sy1) * sourceRowStride +
                                           static_cast<size_t>(sx0) * static_cast<size_t>(channels) +
                                           static_cast<size_t>(c)];
                const double p11 = pSource[static_cast<size_t>(sy1) * sourceRowStride +
                                           static_cast<size_t>(sx1) * static_cast<size_t>(channels) +
                                           static_cast<size_t>(c)];
                const double top = p00 + (p10 - p00) * vx;
                const double bot = p01 + (p11 - p01) * vx;
                const double val = top + (bot - top) * vy;
                pTarget[static_cast<size_t>(ty) * targetRowStride +
                        static_cast<size_t>(tx) * static_cast<size_t>(channels) +
                        static_cast<size_t>(c)] =
                    static_cast<uint8_t>(std::clamp(val, 0.0, 255.0));
            }
        }
    }
}

} // namespace

void blitImage(DecodedImage& target,
               const Rectangle& targetRectangle,
               const DecodedImage& source,
               const Rectangle& sourceRectangle,
               const std::optional<Rectangle>& sourceSubset) {
    // Projection-space intersection — compute directly without
    // antimeridian wrapping (computeIntersection adds kTwoPi for
    // geographic coords, which corrupts projected meters).
    const Rectangle& srcRect = sourceSubset.value_or(sourceRectangle);
    const double oWest = std::max(targetRectangle.west(), srcRect.west());
    const double oEast = std::min(targetRectangle.east(), srcRect.east());
    const double oSouth = std::max(targetRectangle.south(), srcRect.south());
    const double oNorth = std::min(targetRectangle.north(), srcRect.north());
    if (oWest >= oEast || oSouth >= oNorth) return;
    const Rectangle overlap(oWest, oSouth, oEast, oNorth);

    const PixelRectangle dst =
        computePixelRectangle(target, targetRectangle, overlap);
    const PixelRectangle src =
        computePixelRectangle(source, sourceRectangle, overlap);
    if (dst.width <= 0 || dst.height <= 0 ||
        src.width <= 0 || src.height <= 0) {
        return;
    }

    const size_t targetBytesPerPixel =
        static_cast<size_t>(target.channels) *
        static_cast<size_t>(target.bytesPerChannel);
    const size_t sourceBytesPerPixel =
        static_cast<size_t>(source.channels) *
        static_cast<size_t>(source.bytesPerChannel);
    const size_t sourceRowStride =
        static_cast<size_t>(source.width) * sourceBytesPerPixel;
    const size_t targetRowStride =
        static_cast<size_t>(target.width) * targetBytesPerPixel;
    // Alias for backward compat in bilinear resize section below
    const size_t bytesPerPixel = targetBytesPerPixel;

    uint8_t* pTargetRow = target.pixels.data() +
        static_cast<size_t>(dst.y) * targetRowStride +
        static_cast<size_t>(dst.x) * targetBytesPerPixel;
    const uint8_t* pSourceRow = source.pixels.data() +
        static_cast<size_t>(src.y) * sourceRowStride +
        static_cast<size_t>(src.x) * sourceBytesPerPixel;

    if (src.width == dst.width && src.height == dst.height) {
        // Same size: row-wise memcpy (cesium-native unsafeBlitImage path)
        unsafeBlitImage(pTargetRow, targetRowStride,
                        static_cast<size_t>(target.channels),
                        target.bytesPerChannel,
                        pSourceRow, sourceRowStride,
                        static_cast<size_t>(source.channels),
                        source.bytesPerChannel,
                        static_cast<size_t>(dst.width),
                        static_cast<size_t>(dst.height),
                        bytesPerPixel);
    } else {
        // Different size: bilinear interpolation for 1-byte-per-channel
        if (target.bytesPerChannel != 1 || source.bytesPerChannel != 1) {
            return; // Not supported
        }
        const int channels = target.channels;
        // Source channel count may differ; for alpha fill go channel-by-channel.
        if (source.channels < channels) {
            // Fall back to per-channel nearest-neighbor when source has fewer
            // channels — this path is rare (e.g. RGB source → RGBA target).
            // For the common case both match.
            for (int y = 0; y < dst.height; ++y) {
                const int sy = std::clamp(
                    src.y + static_cast<int>(
                        (static_cast<int64_t>(y) * src.height) / dst.height),
                    0, source.height - 1);
                const int dy = dst.y + y;
                if (dy < 0 || dy >= target.height) continue;
                uint8_t* pTarget = target.pixels.data() +
                    static_cast<size_t>(dy) * targetRowStride +
                    static_cast<size_t>(dst.x) * bytesPerPixel;
                const uint8_t* pSrc = source.pixels.data() +
                    static_cast<size_t>(sy) * sourceRowStride +
                    static_cast<size_t>(src.x) * bytesPerPixel;
                for (int x = 0; x < dst.width; ++x) {
                    const int sx = std::clamp(
                        src.x + static_cast<int>(
                            (static_cast<int64_t>(x) * src.width) / dst.width),
                        0, source.width - 1);
                    for (int c = 0; c < channels; ++c) {
                        if (c == 3 && source.channels < 4) {
                            pTarget[static_cast<size_t>(x) * bytesPerPixel + 3] = 0xFF;
                        } else {
                            pTarget[static_cast<size_t>(x) * bytesPerPixel + c] =
                                pSrc[static_cast<size_t>(sx - src.x) * bytesPerPixel +
                                     static_cast<size_t>(std::min(c, source.channels - 1))];
                        }
                    }
                }
            }
        } else {
            // Same channel count: bilinear resize directly into target rectangle
            unsafeBilinearResize(pTargetRow,
                                 dst.width, dst.height, targetRowStride,
                                 pSourceRow,
                                 src.width, src.height, sourceRowStride,
                                 channels);
        }
    }
}

RasterOverlayTileProvider::CompositeImageResult combineQuadtreeSourceImages(
    const TileScheme& scheme,
    const Rectangle& targetBounds,
    std::vector<RasterSourceResult>&& sources) {
    std::vector<std::string> diagnostics;
    std::vector<std::string> credits;
    for (RasterSourceResult& source : sources) {
        diagnostics.insert(
            diagnostics.end(),
            std::make_move_iterator(source.diagnostics.begin()),
            std::make_move_iterator(source.diagnostics.end()));
        appendCredits(credits, source.credits);
    }
    sources.erase(
        std::remove_if(sources.begin(), sources.end(),
                       [](const RasterSourceResult& source) {
                           return !source.image ||
                                  !isRasterCompositeSourceImage(*source.image);
                       }),
        sources.end());
    if (sources.empty()) {
        RasterOverlayTileProvider::CompositeImageResult result;
        result.diagnostics = std::move(diagnostics);
        result.credits = std::move(credits);
        return result;
    }

    // cesium-native: project ALL rectangles to provider projection space at
    // the pipeline entry boundary so that X and Y are symmetric throughout.
    const RasterOverlayProjection projType = projectionForScheme(scheme);
    const Rectangle projectedTarget = projectGeographicToProvider(targetBounds, projType);

    double projectedWidthPerPixel = std::numeric_limits<double>::max();
    double projectedHeightPerPixel = std::numeric_limits<double>::max();
    for (const RasterSourceResult& source : sources) {
        const Rectangle projectedSource = projectGeographicToProvider(source.bounds, projType);
        projectedWidthPerPixel = std::min(
            projectedWidthPerPixel,
            projectedSource.computeWidth() / static_cast<double>(source.image->width));
        projectedHeightPerPixel = std::min(
            projectedHeightPerPixel,
            projectedSource.computeHeight() / static_cast<double>(source.image->height));
    }
    if (projectedWidthPerPixel <= 0.0 || projectedHeightPerPixel <= 0.0 ||
        !std::isfinite(projectedWidthPerPixel) ||
        !std::isfinite(projectedHeightPerPixel)) {
        RasterOverlayTileProvider::CompositeImageResult result;
        result.image = std::make_unique<DecodedImage>();
        result.rectangle = targetBounds;
        result.diagnostics = std::move(diagnostics);
        result.credits = std::move(credits);
        return result;
    }

    // Build projected source list for measureCombinedImage — must be in the
    // same projection space as projectedTarget.
    std::vector<ProjectedSource> projectedSources;
    projectedSources.reserve(sources.size());
    for (const RasterSourceResult& src : sources) {
        ProjectedSource ps;
        ps.bounds = projectGeographicToProvider(src.bounds, projType);
        if (src.sourceSubset) {
            ps.sourceSubset = projectGeographicToProvider(*src.sourceSubset, projType);
        }
        projectedSources.push_back(std::move(ps));
    }

    CombinedImageMeasurements measurements = measureCombinedImage(
        projectedTarget,
        projectedSources,
        sources,
        projectedWidthPerPixel,
        projectedHeightPerPixel);

    if (measurements.width <= 0 || measurements.height <= 0 ||
        measurements.channels <= 0) {
        RasterOverlayTileProvider::CompositeImageResult result;
        result.rectangle = targetBounds;
        result.moreDetailAvailable =
            RasterOverlayTile::MoreDetailAvailable::Yes;
        result.diagnostics = std::move(diagnostics);
        result.credits = std::move(credits);
        return result;
    }
    auto output = std::make_unique<DecodedImage>();
    output->width = measurements.width;
    output->height = measurements.height;
    output->channels = measurements.channels;
    output->bytesPerChannel = measurements.bytesPerChannel;
    output->pixels.resize(static_cast<size_t>(output->width) *
                          static_cast<size_t>(output->height) *
                          static_cast<size_t>(output->channels) *
                          static_cast<size_t>(output->bytesPerChannel),
                          0);

    for (size_t i = 0; i < sources.size(); ++i) {
        const RasterSourceResult& source = sources[i];
        const Rectangle& projectedSource = projectedSources[i].bounds;
        const std::optional<Rectangle>& projectedSubset = projectedSources[i].sourceSubset;
        blitImage(*output,
                  measurements.rectangle,    // already in projection space
                  *source.image,
                  projectedSource,            // projected — symmetric with measurements.rectangle
                  projectedSubset);
    }

    // cesium-native: combineImages returns projection-space rectangle.
    // Ours unprojects back to geographic to match downstream expectations
    // (RasterOverlayTile::getRectangle, UV transform, etc.).
    RasterOverlayTileProvider::CompositeImageResult result;
    result.image = std::move(output);
    result.rectangle = unprojectProviderToGeographic(measurements.rectangle, projType);
    const bool moreDetailAvailable = std::any_of(
        sources.begin(),
        sources.end(),
        [](const RasterSourceResult& source) {
            return !source.sourceSubset.has_value() &&
                   source.moreDetailAvailable == RasterOverlayTile::MoreDetailAvailable::Yes;
        });
    result.moreDetailAvailable =
        moreDetailAvailable
            ? RasterOverlayTile::MoreDetailAvailable::Yes
            : RasterOverlayTile::MoreDetailAvailable::No;
    result.diagnostics = std::move(diagnostics);
    result.credits = std::move(credits);
    return result;
}

using MappedSourceLoadSuccess =
    std::function<void(std::unique_ptr<DecodedImage>,
                       std::shared_ptr<const DecodedImage>,
                       Rectangle,
                       RasterOverlayTile::MoreDetailAvailable,
                       std::vector<std::string>,
                       std::vector<std::string>)>;
using MappedSourceLoadFailure = std::function<void(std::vector<std::string>)>;

RasterOverlayTileProvider::CompositeImageResult composeMappedSourceImageSet(
    const TileScheme& scheme,
    const Rectangle& targetBounds,
    std::vector<RasterSourceResult>&& sources,
    bool emptyWhenOnlyAncestorFallback) {
    const bool haveAnyUsefulImageData =
        !emptyWhenOnlyAncestorFallback ||
        hasNonAncestorRasterSourceImage(sources);
    if (!haveAnyUsefulImageData) {
        RasterOverlayTileProvider::CompositeImageResult result;
        result.image = std::make_unique<DecodedImage>();
        result.moreDetailAvailable =
            RasterOverlayTile::MoreDetailAvailable::No;
        for (RasterSourceResult& source : sources) {
            result.diagnostics.insert(
                result.diagnostics.end(),
                std::make_move_iterator(source.diagnostics.begin()),
                std::make_move_iterator(source.diagnostics.end()));
            appendCredits(result.credits, source.credits);
        }
        return result;
    }

    return combineQuadtreeSourceImages(
        scheme,
        targetBounds,
        std::move(sources));
}

bool isMappedRasterCacheKey(const std::string& cacheKey) {
    return cacheKey.rfind("mapped-raster/", 0) == 0;
}

bool isEpochMappedRasterCacheKey(const std::string& cacheKey) {
    return cacheKey.rfind("mapped-raster/epoch/", 0) == 0;
}

bool isTransientRasterSourceFailure(
    const std::vector<std::string>& diagnostics) {
    return std::any_of(
        diagnostics.begin(),
        diagnostics.end(),
        [](const std::string& diagnostic) {
            return diagnostic.find(
                       "Raster source tile request threw before completion") !=
                   std::string::npos;
        });
}

} // namespace

RasterOverlayTileProvider::QuadtreeSourcePlan
RasterOverlayTileProvider::buildQuadtreeSourcePlan(
    const TileScheme& scheme,
    const ImageryProvider& provider,
    const RasterTextureUploader* uploader,
    const Rectangle& geometryBounds,
    const Rectangle& sourceBounds,
    double targetScreenPixelsX,
    double targetScreenPixelsY,
    double maximumScreenSpaceError,
    int maximumTextureSize,
    int minimumLevel,
    int maximumLevel) {
    QuadtreeSourcePlan plan;
    TileRange range;
    plan.sourceZoom = chooseQuadtreeSourceZoom(
        scheme,
        provider,
        uploader,
        geometryBounds,
        sourceBounds,
        targetScreenPixelsX,
        targetScreenPixelsY,
        maximumScreenSpaceError,
        maximumTextureSize,
        minimumLevel,
        maximumLevel,
        &range);
    plan.minX = range.minX;
    plan.minY = range.minY;
    plan.maxX = range.maxX;
    plan.maxY = range.maxY;
    const TileCoverage coverage = computeCoverage(
        scheme,
        sourceBounds,
        plan.sourceZoom);
    plan.sourceKeys.reserve(
        static_cast<size_t>(std::min<int64_t>(
            coverage.count64(),
            kMaximumSourcePlanReserve)));
    for (const TileRange& coveredRange : coverage.ranges) {
        for (int x = coveredRange.minX; x <= coveredRange.maxX; ++x) {
            for (int y = coveredRange.minY; y <= coveredRange.maxY; ++y) {
                TileKey sourceKey{scheme.id(), plan.sourceZoom, x, y};
                if (matchesProviderQuadtreeRange(provider, sourceKey) &&
                    rectanglesOverlapWithArea(
                        scheme.tileToRectangle(sourceKey),
                        sourceBounds)) {
                    plan.sourceKeys.push_back(sourceKey);
                }
            }
        }
    }
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
                if (matchesProviderQuadtreeRange(provider, sourceKey) &&
                    rectanglesOverlapWithArea(
                        scheme.tileToRectangle(sourceKey),
                        sourceBounds)) {
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
    return plan;
}

struct RasterOverlayTileProvider::QuadtreeSourceAssetDepot
    : public std::enable_shared_from_this<QuadtreeSourceAssetDepot> {
    using SourceReady = std::function<void(RasterSourceResult&&)>;

    QuadtreeSourceAssetDepot(ImageryProvider& imageryProvider,
                             const TileScheme& tileScheme,
                             std::shared_ptr<ProviderAsyncState> asyncState,
                             int minimumSourceLevel,
                             int maximumSourceLevel)
        : provider(imageryProvider)
        , scheme(tileScheme)
        , state(std::move(asyncState))
        , cache(state->sourceTileDepotCache)
        , cacheLru(state->sourceTileDepotCacheLru)
        , cacheBytes(state->sourceTileDepotCacheBytes)
        , cacheGeneration(state->sourceTileDepotGeneration)
        , depotEpoch(state->sourceTileDepotEpoch)
        , inFlight(state->sourceTileDepotInFlight)
        , cacheMutex(state->mutex)
        , minimumLevel(minimumSourceLevel)
        , maximumLevel(maximumSourceLevel) {}

    void requestSource(
        const TileKey& requestedKey,
        const TileKey& originalKey,
        bool ancestorFallback,
        bool shareInFlight,
        const std::function<void()>& onSourceIssued,
        const std::function<void()>& onSourceFinished,
        const std::function<void()>& onSourceFailed,
        SourceReady onReady,
        std::vector<TileKey> fallbackInFlightKeys = {}) {
        std::optional<RasterSourceResult> cachedSource;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            const std::string originalCacheKey = depotCacheKey(originalKey);
            auto it = cache.find(originalCacheKey);
            if (it == cache.end() && ancestorFallback) {
                it = cache.find(depotCacheKey(requestedKey));
            }
            if (it != cache.end() &&
                (it->second.image || it->second.terminalFailure)) {
                touchCachedSource(it->first, it->second);
                RasterSourceResult source;
                source.key = it->second.key;
                source.bounds = it->second.bounds;
                source.image = it->second.image;
                source.sourceSubset = ancestorFallback
                    ? std::optional<Rectangle>(
                          scheme.tileToRectangle(originalKey))
                    : it->second.sourceSubset;
                source.moreDetailAvailable = it->second.moreDetailAvailable;
                source.diagnostics = it->second.diagnostics;
                source.credits = it->second.credits;
                source.terminalFailure = it->second.terminalFailure;
                cachedSource = std::move(source);
            }
        }
        if (cachedSource) {
            if (ancestorFallback) {
                auto completed = std::make_shared<SourceTileAsset>(
                    sourceAssetFromResult(*cachedSource));
                if (finishInFlightSource(originalKey, completed) > 0) {
                    return;
                }
            }
            if (onReady) {
                onReady(std::move(*cachedSource));
            }
            return;
        }

        auto self = shared_from_this();
        if (shareInFlight) {
            const std::string inFlightKey = depotCacheKey(originalKey);
            auto waiter =
                [self, originalKey, ancestorFallback, onReady](
                    InFlightSourceTileAsset::Result cached) mutable {
                    if (onReady) {
                        onReady(self->rasterSourceResultFromAsset(
                            cached,
                            originalKey,
                            ancestorFallback));
                    }
                };
            {
                std::lock_guard<std::mutex> lock(cacheMutex);
                auto [it, inserted] =
                    inFlight.try_emplace(inFlightKey, InFlightSourceTileAsset{});
                it->second.waiters.push_back(std::move(waiter));
                if (!inserted) {
                    return;
                }
            }
        }

        if (ancestorFallback) {
            const std::string fallbackInFlightKey =
                depotCacheKey(requestedKey);
            auto waiter =
                [self,
                 originalKey,
                 ancestorFallback,
                 onReady,
                 fallbackInFlightKeys](
                    InFlightSourceTileAsset::Result cached) mutable {
                    RasterSourceResult source =
                        self->rasterSourceResultFromAsset(
                        cached,
                        originalKey,
                        ancestorFallback);
                    if (cached) {
                        auto originalCompleted =
                            std::make_shared<SourceTileAsset>(
                                self->sourceAssetFromResult(source));
                        self->finishInFlightSource(
                            originalKey,
                            originalCompleted);
                        for (const TileKey& key : fallbackInFlightKeys) {
                            self->finishInFlightSource(key, cached);
                        }
                    } else if (onReady) {
                        onReady(std::move(source));
                    }
                };
            {
                std::lock_guard<std::mutex> lock(cacheMutex);
                auto [it, inserted] =
                    inFlight.try_emplace(
                        fallbackInFlightKey,
                        InFlightSourceTileAsset{});
                if (!inserted) {
                    it->second.waiters.push_back(std::move(waiter));
                    return;
                }
            }
            fallbackInFlightKeys.push_back(requestedKey);
        }

        if (onSourceIssued) {
            onSourceIssued();
        }
        CancellationToken token;
        const std::vector<TileKey> exceptionInFlightKeys =
            fallbackInFlightKeys;
        auto callback =
            [self,
             requestedKey,
             originalKey,
             ancestorFallback,
             onSourceIssued,
             onSourceFinished,
             onSourceFailed,
             onReady,
             fallbackInFlightKeys = std::move(fallbackInFlightKeys)](
                const TileKey& loadedKey,
                std::unique_ptr<DecodedImage> image) mutable {
                if (!self->state->alive.load(std::memory_order_acquire)) {
                    if (onSourceFinished) {
                        onSourceFinished();
                    }
                    auto abandoned =
                        self->makeAbandonedSourceResult(originalKey);
                    self->finishInFlightSource(originalKey, abandoned);
                    for (const TileKey& key : fallbackInFlightKeys) {
                        self->finishInFlightSource(key, abandoned);
                    }
                    return;
                }

                if (image) {
                    if (onSourceFinished) {
                        onSourceFinished();
                    }
                    RasterSourceResult source;
                    source.key = loadedKey;
                    source.bounds = self->scheme.tileToRectangle(loadedKey);
                    source.image =
                        std::shared_ptr<const DecodedImage>(std::move(image));
                    source.sourceSubset = ancestorFallback
                        ? std::optional<Rectangle>(
                              self->scheme.tileToRectangle(originalKey))
                        : std::nullopt;
                    source.moreDetailAvailable =
                        loadedKey.z < self->maximumLevel
                            ? RasterOverlayTile::MoreDetailAvailable::Yes
                            : RasterOverlayTile::MoreDetailAvailable::No;
                    const std::string attribution =
                        self->provider.attribution();
                    if (!attribution.empty()) {
                        source.credits.push_back(attribution);
                    }
                    auto completed = std::make_shared<SourceTileAsset>(
                        self->sourceAssetFromResult(source));
                    InFlightSourceTileAsset::Result directCompleted =
                        completed;
                    if (!ancestorFallback) {
                        self->cacheSource(originalKey, source);
                    }
                    if (loadedKey != originalKey) {
                        RasterSourceResult directSource;
                        directSource.key = loadedKey;
                        directSource.bounds = source.bounds;
                        directSource.image = source.image;
                        directSource.sourceSubset = std::nullopt;
                        directSource.moreDetailAvailable =
                            source.moreDetailAvailable;
                        directSource.diagnostics = source.diagnostics;
                        directSource.credits = source.credits;
                        self->cacheSource(loadedKey, directSource);
                        directCompleted = std::make_shared<SourceTileAsset>(
                            self->sourceAssetFromResult(directSource));
                    }
                    self->finishInFlightSource(originalKey, completed);
                    for (const TileKey& key : fallbackInFlightKeys) {
                        self->finishInFlightSource(key, directCompleted);
                    }
                    return;
                }

                if (onSourceFailed) {
                    onSourceFailed();
                }
                if (requestedKey.z > self->minimumLevel) {
                    const TileKey parentKey = parentTileKey(requestedKey);
                    if (onSourceFinished) {
                        onSourceFinished();
                    }
                    if (!self->state->alive.load(std::memory_order_acquire)) {
                        auto abandoned =
                            self->makeAbandonedSourceResult(originalKey);
                        self->finishInFlightSource(originalKey, abandoned);
                        for (const TileKey& key : fallbackInFlightKeys) {
                            self->finishInFlightSource(key, abandoned);
                        }
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(
                            self->state->mutex);
                        self->state->pendingSourceFallbacks.push_back(
                            PendingSourceFallback{
                                originalKey,
                                [self,
                                 parentKey,
                                 originalKey,
                                 onSourceIssued,
                                 onSourceFinished,
                                 onSourceFailed,
                                 onReady,
                                 fallbackInFlightKeys]() mutable {
                                    // requestSource retains onSourceIssued in
                                    // its async completion callback. A stack
                                    // reference here becomes dangling when a
                                    // failed parent queues another fallback.
                                    auto issued = std::make_shared<int>(0);
                                    self->requestSource(
                                        parentKey,
                                        originalKey,
                                        true,
                                        false,
                                        [issued, onSourceIssued]() {
                                            ++(*issued);
                                            if (onSourceIssued) {
                                                onSourceIssued();
                                            }
                                        },
                                        onSourceFinished,
                                        onSourceFailed,
                                        std::move(onReady),
                                        std::move(fallbackInFlightKeys));
                                    return *issued;
                                }});
                    }
                    return;
                }

                if (onSourceFinished) {
                    onSourceFinished();
                }
                auto failed = self->cacheTerminalFailure(originalKey);
                self->finishInFlightSource(originalKey, failed);
                for (const TileKey& key : fallbackInFlightKeys) {
                    self->finishInFlightSource(key, failed);
                }
            };
        try {
            provider.requestTile(
                requestedKey,
                token,
                std::move(callback));
        } catch (...) {
            if (onSourceFailed) {
                onSourceFailed();
            }
            if (onSourceFinished) {
                onSourceFinished();
            }
            SourceTileAsset failed;
            failed.key = originalKey;
            failed.bounds = scheme.tileToRectangle(originalKey);
            failed.moreDetailAvailable =
                RasterOverlayTile::MoreDetailAvailable::No;
            failed.diagnostics.push_back(
                "Raster source tile request threw before completion");
            failed.terminalFailure = true;
            auto transientFailure =
                std::make_shared<SourceTileAsset>(std::move(failed));
            finishInFlightSource(originalKey, transientFailure);
            for (const TileKey& key : exceptionInFlightKeys) {
                finishInFlightSource(key, transientFailure);
            }
        }
    }

    bool wouldIssueNewRequest(const TileKey& originalKey) const {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const std::string key = depotCacheKey(originalKey);
        auto cached = cache.find(key);
        if (cached != cache.end() &&
            (cached->second.image || cached->second.terminalFailure)) {
            return false;
        }
        return inFlight.find(key) == inFlight.end();
    }

    void abandonInFlightSource(const TileKey& originalKey) {
        finishInFlightSource(
            originalKey,
            makeAbandonedSourceResult(originalKey));
    }

private:
    RasterSourceResult rasterSourceResultFromAsset(
        const InFlightSourceTileAsset::Result& cached,
        const TileKey& originalKey,
        bool ancestorFallback) const {
        if (!cached) {
            return RasterSourceResult{};
        }
        RasterSourceResult source;
        source.key = cached->key;
        source.bounds = cached->bounds;
        source.image = cached->image;
        source.sourceSubset = ancestorFallback
            ? std::optional<Rectangle>(scheme.tileToRectangle(originalKey))
            : cached->sourceSubset;
        source.moreDetailAvailable = cached->moreDetailAvailable;
        source.diagnostics = cached->diagnostics;
        source.credits = cached->credits;
        source.terminalFailure = cached->terminalFailure;
        return source;
    }

    SourceTileAsset sourceAssetFromResult(
        const RasterSourceResult& source) const {
        SourceTileAsset cached;
        cached.key = source.key;
        cached.bounds = source.bounds;
        if (source.image) {
            cached.image = source.image;
            cached.sizeBytes = decodedImageSizeBytes(*source.image);
        }
        cached.sourceSubset = source.sourceSubset;
        cached.moreDetailAvailable = source.moreDetailAvailable;
        cached.diagnostics = source.diagnostics;
        cached.credits = source.credits;
        cached.terminalFailure = source.terminalFailure;
        return cached;
    }

    InFlightSourceTileAsset::Result makeAbandonedSourceResult(
        const TileKey& requestedKey) const {
        SourceTileAsset abandoned;
        abandoned.key = requestedKey;
        abandoned.bounds = scheme.tileToRectangle(requestedKey);
        abandoned.moreDetailAvailable =
            RasterOverlayTile::MoreDetailAvailable::No;
        abandoned.diagnostics.push_back(
            "Raster source tile abandoned after provider destruction");
        abandoned.terminalFailure = true;
        return std::make_shared<SourceTileAsset>(std::move(abandoned));
    }

    InFlightSourceTileAsset::Result cacheTerminalFailure(
        const TileKey& requestedKey) {
        SourceTileAsset failed;
        failed.key = requestedKey;
        failed.bounds = scheme.tileToRectangle(requestedKey);
        failed.moreDetailAvailable =
            RasterOverlayTile::MoreDetailAvailable::No;
        failed.diagnostics.push_back(
            "Raster source tile failed after exhausting parent fallback");
        failed.terminalFailure = true;
        failed.sizeBytes = 1;

        auto cached = std::make_shared<SourceTileAsset>(failed);
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (state->sourceTileDepotEpoch != depotEpoch) {
            return cached;
        }
        const int64_t cacheBudgetBytes = std::max<int64_t>(
            0,
            state->subTileCacheBytes - state->pendingUploadBytes);
        if (cacheBudgetBytes <= 0) {
            cache.clear();
            cacheLru.clear();
            cacheBytes = 0;
            return cached;
        }
        const std::string key = depotCacheKey(requestedKey);
        auto existing = cache.find(key);
        if (existing != cache.end()) {
            cacheBytes -= existing->second.sizeBytes;
        }
        failed.generation = ++cacheGeneration;
        cacheBytes += failed.sizeBytes;
        trackPeakBytes(cacheBytes, state->peakSourceTileDepotCacheBytes);
        cacheLru.emplace_back(key, failed.generation);
        compactCacheLruIfNeeded();
        cache[key] = failed;
        pruneCacheToBudget(cacheBudgetBytes);
        return cached;
    }

    size_t finishInFlightSource(const TileKey& originalKey,
                                InFlightSourceTileAsset::Result source) {
        std::vector<std::function<void(InFlightSourceTileAsset::Result)>>
            waiters;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            auto it = inFlight.find(depotCacheKey(originalKey));
            if (it != inFlight.end()) {
                waiters = std::move(it->second.waiters);
                inFlight.erase(it);
            }
        }
        for (auto& waiter : waiters) {
            waiter(source);
        }
        return waiters.size();
    }

    void cacheSource(const TileKey& requestedKey,
                     const RasterSourceResult& source) {
        if (!source.image) return;
        SourceTileAsset cached = sourceAssetFromResult(source);
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (state->sourceTileDepotEpoch != depotEpoch) {
            return;
        }
        const int64_t cacheBudgetBytes = std::max<int64_t>(
            0,
            state->subTileCacheBytes - state->pendingUploadBytes);
        if (cacheBudgetBytes <= 0) {
            cache.clear();
            cacheLru.clear();
            cacheBytes = 0;
            return;
        }
        const std::string key = depotCacheKey(requestedKey);
        auto existing = cache.find(key);
        if (existing != cache.end()) {
            cacheBytes -= existing->second.sizeBytes;
        }
        cached.generation = ++cacheGeneration;
        cacheBytes += cached.sizeBytes;
        trackPeakBytes(cacheBytes, state->peakSourceTileDepotCacheBytes);
        cacheLru.emplace_back(key, cached.generation);
        compactCacheLruIfNeeded();
        cache[key] = std::move(cached);
        pruneCacheToBudget(cacheBudgetBytes);
    }

    void touchCachedSource(const std::string& key, SourceTileAsset& source) {
        source.generation = ++cacheGeneration;
        cacheLru.emplace_back(key, source.generation);
        compactCacheLruIfNeeded();
    }

    void pruneCacheToBudget(int64_t cacheBudgetBytes) {
        while (cacheBytes > cacheBudgetBytes && !cacheLru.empty()) {
            auto [key, generation] = cacheLru.front();
            cacheLru.pop_front();
            auto it = cache.find(key);
            if (it == cache.end() || it->second.generation != generation) {
                continue;
            }
            cacheBytes -= it->second.sizeBytes;
            cache.erase(it);
        }
        if (cacheBytes < 0) {
            cacheBytes = 0;
        }
    }

    void compactCacheLruIfNeeded() {
        constexpr size_t kLruSlackEntries = 32;
        const size_t liveEntries = cache.size();
        if (cacheLru.size() <= liveEntries + kLruSlackEntries) {
            return;
        }
        if (liveEntries > 0 &&
            cacheLru.size() <= liveEntries * 2 + kLruSlackEntries) {
            return;
        }

        std::vector<std::pair<std::string, uint64_t>> compactedEntries;
        compactedEntries.reserve(liveEntries);
        for (const auto& [key, source] : cache) {
            compactedEntries.emplace_back(key, source.generation);
        }
        std::sort(
            compactedEntries.begin(),
            compactedEntries.end(),
            [](const auto& left, const auto& right) {
                return std::tie(left.second, left.first) <
                       std::tie(right.second, right.first);
            });

        std::deque<std::pair<std::string, uint64_t>> compactedLru;
        compactedLru.insert(
            compactedLru.end(),
            std::make_move_iterator(compactedEntries.begin()),
            std::make_move_iterator(compactedEntries.end()));
        cacheLru.swap(compactedLru);
    }

    ImageryProvider& provider;
    const TileScheme& scheme;
    std::shared_ptr<ProviderAsyncState> state;
    std::unordered_map<std::string, SourceTileAsset>& cache;
    std::deque<std::pair<std::string, uint64_t>>& cacheLru;
    int64_t& cacheBytes;
    uint64_t& cacheGeneration;
    uint64_t depotEpoch = 0;
    std::unordered_map<std::string, InFlightSourceTileAsset>& inFlight;
    std::mutex& cacheMutex;
    int minimumLevel = 0;
    int maximumLevel = 0;

    std::string depotCacheKey(const TileKey& key) const {
        return sourceCacheKey(depotEpoch, key);
    }
};

struct RasterOverlayTileProvider::MappedSourceImageSet
    : public std::enable_shared_from_this<MappedSourceImageSet> {
    MappedSourceImageSet(const TileScheme& tileScheme,
                             std::shared_ptr<ProviderAsyncState> asyncState,
                             std::shared_ptr<std::atomic<bool>>
                                 throttleSlotReleased,
                             std::shared_ptr<QuadtreeSourceAssetDepot>
                                 sourceDepot,
                             RasterSourceTileMapping sourceTileMapping,
                             Rectangle bounds,
                             RasterOverlayProjection outputProjection,
                             int maximumSourceLevel,
                             bool emptyWhenOnlyAncestorFallback,
                             bool allowDirectTerminalFailure,
                             MappedSourceLoadSuccess success,
                             MappedSourceLoadFailure failure)
        : scheme(createAsyncSchemeSnapshot(tileScheme))
        , state(std::move(asyncState))
        , slotReleased(std::move(throttleSlotReleased))
        , depot(std::move(sourceDepot))
        , sourceTiles(std::move(sourceTileMapping))
        , targetBounds(bounds)
        , projection(outputProjection)
        , maximumLevel(maximumSourceLevel)
        , returnEmptyForAncestorOnly(emptyWhenOnlyAncestorFallback)
        , directTerminalFailure(allowDirectTerminalFailure)
        , onSuccess(std::move(success))
        , onFailure(std::move(failure))
        , remaining(sourceTiles.budgetUnits()) {
        sources.reserve(sourceTiles.sourceKeys.size());
        sourceIssued.assign(sourceTiles.sourceKeys.size(), false);
    }

    int issueSome(int maxNewRequests,
                  const std::function<void()>& onSourceIssued,
                  const std::function<void()>& onSourceFinished,
                  const std::function<void()>& onSourceFailed) {
        auto issued = std::make_shared<int>(0);
        std::vector<TileKey> sourceKeys;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (completed) {
                return 0;
            }
            int remainingNewRequests = maxNewRequests;
            for (size_t i = 0; i < sourceTiles.sourceKeys.size(); ++i) {
                if (sourceIssued[i]) {
                    continue;
                }
                const TileKey& sourceKey = sourceTiles.sourceKeys[i];
                const bool needsNewRequest =
                    depot->wouldIssueNewRequest(sourceKey);
                if (needsNewRequest && remainingNewRequests <= 0) {
                    continue;
                }
                if (needsNewRequest) {
                    --remainingNewRequests;
                }
                sourceKeys.push_back(sourceKey);
                sourceIssued[i] = true;
            }
        }
        for (const TileKey& sourceKey : sourceKeys) {
            auto self = shared_from_this();
            depot->requestSource(
                sourceKey,
                sourceKey,
                false,
                true,
                [issued, onSourceIssued]() {
                    ++(*issued);
                    onSourceIssued();
                },
                onSourceFinished,
                onSourceFailed,
                [self](RasterSourceResult&& source) {
                    self->finishOneSource(std::move(source));
                });
        }
        return *issued;
    }

    bool hasUnissuedSources() const {
        std::lock_guard<std::mutex> lock(mutex);
        return !completed &&
               std::any_of(
                   sourceIssued.begin(),
                   sourceIssued.end(),
                   [](bool issued) { return !issued; });
    }

    bool isComplete() const {
        std::lock_guard<std::mutex> lock(mutex);
        return completed;
    }

    void markAbandoned() {
        std::lock_guard<std::mutex> lock(mutex);
        completed = true;
        remaining = 0;
        std::fill(sourceIssued.begin(), sourceIssued.end(), true);
        sources.clear();
    }

    void releaseThrottleSlotOnce() {
        releaseRasterThrottleSlotOnce(
            *slotReleased,
            state->activeRasterTileLoads);
    }

private:
    void finishOneSource(RasterSourceResult&& source) {
        bool finished = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (completed) {
                return;
            }
            if (isResolvedRasterSourceResult(source)) {
                sources.push_back(std::move(source));
            }
            --remaining;
            finished = remaining == 0;
            completed = finished;
        }

        if (!finished) return;

        std::vector<RasterSourceResult> completedSources;
        {
            std::lock_guard<std::mutex> lock(mutex);
            completedSources = std::move(sources);
        }

        if (directTerminalFailure &&
            completedSources.size() == 1 &&
            sourceTiles.sourceKeys.size() == 1 &&
            completedSources.front().terminalFailure &&
            !completedSources.front().image &&
            !completedSources.front().sourceSubset.has_value() &&
            rectanglesEqualForDirectRasterTile(
                targetBounds,
                completedSources.front().bounds)) {
            onFailure(std::move(completedSources.front().diagnostics));
            return;
        }

        if (completedSources.size() == 1 &&
            sourceTiles.sourceKeys.size() == 1 &&
            completedSources.front().image &&
            !completedSources.front().sourceSubset.has_value() &&
            rectanglesEqualForDirectRasterTile(
                targetBounds,
                completedSources.front().bounds)) {
            RasterSourceResult& source = completedSources.front();
            const RasterOverlayTile::MoreDetailAvailable moreDetailAvailable =
                source.moreDetailAvailable !=
                        RasterOverlayTile::MoreDetailAvailable::Unknown
                    ? source.moreDetailAvailable
                    : (source.key.z < maximumLevel
                           ? RasterOverlayTile::MoreDetailAvailable::Yes
                           : RasterOverlayTile::MoreDetailAvailable::No);
            onSuccess(
                nullptr,
                source.image,
                projectGeographicToProvider(source.bounds, projection),
                moreDetailAvailable,
                std::move(source.diagnostics),
                std::move(source.credits));
            return;
        }

        auto self = shared_from_this();
        if (!state->alive.load(std::memory_order_acquire)) {
            onFailure({});
            return;
        }
        if (!scheme) {
            onFailure({});
            return;
        }
        state->activeRasterComposeTasks.fetch_add(
            1,
            std::memory_order_relaxed);
        try {
            AsyncSystem::pool().enqueue(
                [self,
                 completedSources = std::move(completedSources)]() mutable {
                    bool completedTileLoad = false;
                    const auto finishAbandonedTileLoad = [&self,
                                                           &completedTileLoad]() {
                        if (completedTileLoad ||
                            self->state->alive.load(
                                std::memory_order_acquire)) {
                            return;
                        }
                        self->releaseThrottleSlotOnce();
                        completedTileLoad = true;
                        self->state->resolveDestructionIfComplete();
                    };
                    const auto finishCompose = [&self]() {
                        self->state->activeRasterComposeTasks.fetch_sub(
                            1,
                            std::memory_order_relaxed);
                        self->state->resolveDestructionIfComplete();
                    };
                    try {
                        CompositeImageResult composed =
                            composeMappedSourceImageSet(
                                *self->scheme,
                                self->targetBounds,
                                std::move(completedSources),
                                self->returnEmptyForAncestorOnly);
                        if (composed.image) {
                            if (self->state->alive.load(
                                    std::memory_order_acquire)) {
                                completedTileLoad = true;
                                self->onSuccess(
                                    std::move(composed.image),
                                    nullptr,
                                    projectGeographicToProvider(
                                        composed.rectangle,
                                        self->projection),
                                    composed.moreDetailAvailable,
                                    std::move(composed.diagnostics),
                                    std::move(composed.credits));
                            }
                        } else {
                            if (self->state->alive.load(
                                    std::memory_order_acquire)) {
                                completedTileLoad = true;
                                self->onFailure(
                                    std::move(composed.diagnostics));
                            }
                        }
                        finishAbandonedTileLoad();
                        finishCompose();
                    } catch (...) {
                        if (self->state->alive.load(
                                std::memory_order_acquire)) {
                            completedTileLoad = true;
                            self->onFailure({});
                        }
                        finishAbandonedTileLoad();
                        finishCompose();
                    }
                });
        } catch (...) {
            state->activeRasterComposeTasks.fetch_sub(
                1,
                std::memory_order_relaxed);
            state->resolveDestructionIfComplete();
            onFailure({});
        }
    }

    std::unique_ptr<TileScheme> scheme;
    std::shared_ptr<ProviderAsyncState> state;
    std::shared_ptr<std::atomic<bool>> slotReleased;
    std::shared_ptr<QuadtreeSourceAssetDepot> depot;
    RasterSourceTileMapping sourceTiles;
    Rectangle targetBounds;
    RasterOverlayProjection projection;
    int maximumLevel = 0;
    bool returnEmptyForAncestorOnly = false;
    bool directTerminalFailure = false;
    MappedSourceLoadSuccess onSuccess;
    MappedSourceLoadFailure onFailure;
    mutable std::mutex mutex;
    int remaining = 0;
    std::vector<bool> sourceIssued;
    bool completed = false;
    std::vector<RasterSourceResult> sources;
};

RasterOverlayTileProvider::CompositeImageResult
RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
    const TileScheme& scheme,
    const Rectangle& targetBounds,
    std::vector<QuadtreeSourceImage>&& publicSources) {
    std::vector<RasterSourceResult> sources;
    sources.reserve(publicSources.size());
    for (auto& source : publicSources) {
        sources.push_back(RasterSourceResult{
            source.key,
            source.bounds,
            std::shared_ptr<const DecodedImage>(std::move(source.image)),
            source.sourceSubset,
            source.moreDetailAvailable,
            std::move(source.diagnostics),
            std::move(source.credits),
            false});
    }
    if (!hasNonAncestorRasterSourceImage(sources)) {
        CompositeImageResult result;
        result.image = std::make_unique<DecodedImage>();
        result.moreDetailAvailable =
            RasterOverlayTile::MoreDetailAvailable::No;
        for (RasterSourceResult& source : sources) {
            result.diagnostics.insert(
                result.diagnostics.end(),
                std::make_move_iterator(source.diagnostics.begin()),
                std::make_move_iterator(source.diagnostics.end()));
        }
        return result;
    }
    return combineQuadtreeSourceImages(
        scheme,
        targetBounds,
        std::move(sources));
}

double RasterOverlayTileProvider::projectedVForLatitude(
    const TileScheme& scheme,
    const Rectangle& bounds,
    double lat) {
    return projectedVForLatitudeInternal(scheme, bounds, lat);
}

int64_t RasterOverlayTileProvider::pendingUploadSizeBytes(
    const PendingUpload& upload) {
    if (upload.image) {
        return decodedImageSizeBytes(*upload.image);
    }
    if (upload.sharedImage) {
        return decodedImageSizeBytes(*upload.sharedImage);
    }
    return 0;
}

void RasterOverlayTileProvider::clearPendingUploads(ProviderAsyncState& state) {
    state.pendingUploads.clear();
    state.pendingUploadBytes = 0;
    enforceSourceDepotBudgetLocked(state);
}

void RasterOverlayTileProvider::enforceSourceDepotBudgetLocked(
    ProviderAsyncState& state) {
    const int64_t totalBudgetBytes = std::max<int64_t>(0, state.subTileCacheBytes);
    const int64_t sourceBudgetBytes = std::max<int64_t>(
        0,
        totalBudgetBytes - state.pendingUploadBytes);
    while (state.sourceTileDepotCacheBytes > sourceBudgetBytes &&
           !state.sourceTileDepotCacheLru.empty()) {
        auto [key, generation] = state.sourceTileDepotCacheLru.front();
        state.sourceTileDepotCacheLru.pop_front();
        auto it = state.sourceTileDepotCache.find(key);
        if (it == state.sourceTileDepotCache.end() ||
            it->second.generation != generation) {
            continue;
        }
        state.sourceTileDepotCacheBytes -= it->second.sizeBytes;
        state.sourceTileDepotCache.erase(it);
    }
    if (state.sourceTileDepotCacheBytes < 0) {
        state.sourceTileDepotCacheBytes = 0;
    }
}

RasterOverlayTileProvider::RasterOverlayTileProvider(ImageryProvider& provider,
                                                     const TileScheme& scheme,
                                                     std::unique_ptr<RasterTextureUploader> textureUploader)
    : provider_(provider)
    , scheme_(scheme)
    , projection_(projectionForScheme(scheme))
    , textureUploader_(std::move(textureUploader)) {
    refreshSourceAssetDepot();
}

RasterOverlayTileProvider::~RasterOverlayTileProvider() {
    asyncState_->alive.store(false, std::memory_order_release);
    std::vector<std::shared_ptr<MappedSourceImageSet>> abandonedSourceSets;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        // pendingUploads 的节流名额已在加载完成入队时释放，直接丢弃
        clearPendingUploads(*asyncState_);
        for (auto& [_, sourceSet] : asyncState_->activeMappedSourceSets) {
            if (sourceSet) {
                abandonedSourceSets.push_back(std::move(sourceSet));
            }
        }
        asyncState_->activeMappedSourceSets.clear();
        asyncState_->activeMappedSourceSetOrder.clear();
        asyncState_->pendingSourceFallbacks.clear();
        asyncState_->inFlightRequests.clear();
    }
    for (const auto& sourceSet : abandonedSourceSets) {
        sourceSet->markAbandoned();
        sourceSet->releaseThrottleSlotOnce();
    }
    asyncState_->resolveDestructionIfComplete();
}

std::shared_future<void>
RasterOverlayTileProvider::getAsyncDestructionCompleteEvent() {
    {
        std::lock_guard<std::mutex> lock(asyncState_->destructionMutex);
        if (!asyncState_->destructionCompletePromise) {
            asyncState_->destructionCompletePromise =
                std::make_shared<std::promise<void>>();
            asyncState_->destructionCompleteFuture =
                asyncState_->destructionCompletePromise->get_future().share();
        }
    }
    asyncState_->resolveDestructionIfComplete();
    return asyncState_->destructionCompleteFuture;
}

void RasterOverlayTileProvider::setReady(bool ready) {
    if (ready_ == ready) {
        return;
    }

    ready_ = ready;
    if (ready_) {
        asyncState_->revision.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    abandonActiveMappedSourceSets();

    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        ++asyncState_->sourceTileDepotEpoch;
        asyncState_->sourceTileDepotCache.clear();
        asyncState_->sourceTileDepotCacheLru.clear();
        asyncState_->sourceTileDepotCacheBytes = 0;
        asyncState_->inFlightRequests.clear();
        asyncState_->activeMappedSourceSetOrder.clear();
        asyncState_->pendingSourceFallbacks.clear();
        // pendingUploads 的节流名额已在加载完成入队时释放，直接丢弃
        clearPendingUploads(*asyncState_);
    }

    for (auto& entry : tiles_) {
        if (!entry.second) {
            continue;
        }
        entry.second->setMoreDetailAvailable(
            RasterOverlayTile::MoreDetailAvailable::No);
        entry.second->setState(RasterOverlayTile::LoadState::Failed);
    }
    tiles_.clear();

    refreshSourceAssetDepot();
    asyncState_->revision.fetch_add(1, std::memory_order_relaxed);
    asyncState_->resolveDestructionIfComplete();
}

void RasterOverlayTileProvider::setOwner(RasterOverlay* owner) {
    owner_ = owner;
    applyOwnerOptions();
}

void RasterOverlayTileProvider::applyOwnerOptions() {
    if (!owner_) {
        return;
    }
    const RasterOverlay::Options& options = owner_->getOptions();
    if (!hasAppliedOwnerOptions_ ||
        appliedOwnerCoverageRectangle_ != options.coverageRectangle) {
        setCoverageRectangle(options.coverageRectangle);
        appliedOwnerCoverageRectangle_ = options.coverageRectangle;
    }
    if (!hasAppliedOwnerOptions_ ||
        appliedOwnerMaximumScreenSpaceError_ !=
            options.maximumScreenSpaceError) {
        setMaximumScreenSpaceError(options.maximumScreenSpaceError);
        appliedOwnerMaximumScreenSpaceError_ =
            options.maximumScreenSpaceError;
    }
    if (!hasAppliedOwnerOptions_ ||
        appliedOwnerMaximumTextureSize_ != options.maximumTextureSize) {
        setMaximumTextureSize(options.maximumTextureSize);
        appliedOwnerMaximumTextureSize_ = options.maximumTextureSize;
    }
    if (!hasAppliedOwnerOptions_ ||
        appliedOwnerSubTileCacheBytes_ != options.subTileCacheBytes) {
        setSubTileCacheBytes(options.subTileCacheBytes);
        appliedOwnerSubTileCacheBytes_ = options.subTileCacheBytes;
    }
    if (!hasAppliedOwnerOptions_ ||
        appliedOwnerMinimumLevel_ != options.minimumZoom ||
        appliedOwnerMaximumLevel_ != options.maximumZoom) {
        setLevelRange(options.minimumZoom, options.maximumZoom);
        appliedOwnerMinimumLevel_ = options.minimumZoom;
        appliedOwnerMaximumLevel_ = options.maximumZoom;
    }
    hasAppliedOwnerOptions_ = true;
}

void RasterOverlayTileProvider::setCoverageRectangle(
    const Rectangle& coverageRectangle) {
    if (coverageRectangle_ == coverageRectangle) {
        return;
    }
    coverageRectangle_ = coverageRectangle;
    const Rectangle effectiveCoverage =
        effectiveCoverageRectangle(scheme_, coverageRectangle_);
    invalidateSourceAssetDepotCache();
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        if (it->first.rfind("mapped-raster/", 0) == 0 || !it->second) {
            ++it;
            continue;
        }
        const Rectangle tileGeographicBounds =
            unprojectProviderToGeographic(
                it->second->getRectangle(),
                projection_);
        const bool stillCovered =
            tileGeographicBounds.computeIntersection(effectiveCoverage)
                .has_value();
        const bool loading =
            it->second->getState() == RasterOverlayTile::LoadState::Loading;
        if (!stillCovered && !loading) {
            it = tiles_.erase(it);
        } else {
            ++it;
        }
    }
    discardPendingUploadsForMissingTiles();
    invalidateMappedRasterTileCache();
}

void RasterOverlayTileProvider::setMaximumScreenSpaceError(
    double maximumScreenSpaceError) {
    const double nextMaximumScreenSpaceError =
        maximumScreenSpaceError > 0.0 ? maximumScreenSpaceError : 2.0;
    if (maximumScreenSpaceError_ == nextMaximumScreenSpaceError) {
        return;
    }
    maximumScreenSpaceError_ = nextMaximumScreenSpaceError;
    invalidateMappedRasterTileCache();
}

void RasterOverlayTileProvider::setMaximumTextureSize(int maximumTextureSize) {
    const int nextMaximumTextureSize =
        maximumTextureSize > 0 ? maximumTextureSize : 2048;
    if (maximumTextureSize_ == nextMaximumTextureSize) {
        return;
    }
    maximumTextureSize_ = nextMaximumTextureSize;
    invalidateMappedRasterTileCache();
    invalidateSourceAssetDepotCache();
}

void RasterOverlayTileProvider::setSubTileCacheBytes(int64_t subTileCacheBytes) {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    asyncState_->subTileCacheBytes = std::max<int64_t>(0, subTileCacheBytes);
    enforceSourceDepotBudgetLocked(*asyncState_);
    if (asyncState_->subTileCacheBytes == 0 ||
        asyncState_->sourceTileDepotCacheBytes < 0) {
        if (asyncState_->subTileCacheBytes == 0) {
            asyncState_->sourceTileDepotCache.clear();
            asyncState_->sourceTileDepotCacheLru.clear();
        }
        asyncState_->sourceTileDepotCacheBytes = 0;
    }
}

void RasterOverlayTileProvider::setLevelRange(int minimumLevel,
                                              int maximumLevel) {
    const int nextMinimumLevel = minimumLevel > 0 ? minimumLevel : 0;
    const int nextMaximumLevel = maximumLevel > 0 ? maximumLevel : 0;
    if (minimumLevel_ == nextMinimumLevel &&
        maximumLevel_ == nextMaximumLevel) {
        return;
    }
    minimumLevel_ = nextMinimumLevel;
    maximumLevel_ = nextMaximumLevel;
    invalidateMappedRasterTileCache();
    invalidateSourceAssetDepotCache();
}

int RasterOverlayTileProvider::getMinimumLevel() const {
    return std::max({scheme_.minZoom(), provider_.minZoom(), minimumLevel_});
}

void RasterOverlayTileProvider::refreshSourceAssetDepot() {
    sourceAssetDepot_ = std::make_shared<QuadtreeSourceAssetDepot>(
        provider_,
        scheme_,
        asyncState_,
        getMinimumLevel(),
        getMaximumLevel());
}

void RasterOverlayTileProvider::invalidateMappedRasterTileCache() {
    ++mappedRasterTileEpoch_;
    abandonActiveMappedSourceSets();
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        auto& pendingUploads = asyncState_->pendingUploads;
        pendingUploads.erase(
            std::remove_if(
                pendingUploads.begin(),
                pendingUploads.end(),
                [state = asyncState_.get()](const PendingUpload& upload) {
                    if (!isMappedRasterCacheKey(upload.cacheKey)) {
                        return false;
                    }
                    state->pendingUploadBytes = std::max<int64_t>(
                        0,
                        state->pendingUploadBytes -
                            pendingUploadSizeBytes(upload));
                    enforceSourceDepotBudgetLocked(*state);
                    return true;
                }),
            pendingUploads.end());
    }
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        if (isMappedRasterCacheKey(it->first) && it->second) {
            it = tiles_.erase(it);
        } else {
            ++it;
        }
    }
    asyncState_->revision.fetch_add(1, std::memory_order_relaxed);
}

void RasterOverlayTileProvider::invalidateDirectRasterTileCache() {
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        if (!isMappedRasterCacheKey(it->first) && it->second) {
            it->second->setMoreDetailAvailable(
                RasterOverlayTile::MoreDetailAvailable::No);
            it->second->setState(RasterOverlayTile::LoadState::Failed);
            it = tiles_.erase(it);
        } else {
            ++it;
        }
    }
    discardPendingUploadsForMissingTiles();
    asyncState_->revision.fetch_add(1, std::memory_order_relaxed);
}

void RasterOverlayTileProvider::invalidateSourceAssetDepotCache() {
    abandonActiveMappedSourceSets();
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        ++asyncState_->sourceTileDepotEpoch;
        asyncState_->sourceTileDepotCache.clear();
        asyncState_->sourceTileDepotCacheLru.clear();
        asyncState_->sourceTileDepotCacheBytes = 0;
    }
    invalidateDirectRasterTileCache();
    refreshSourceAssetDepot();
}

void RasterOverlayTileProvider::abandonActiveMappedSourceSets() {
    std::vector<std::pair<std::string, std::shared_ptr<MappedSourceImageSet>>>
        activeSets;
    std::vector<TileKey> abandonedFallbackSources;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        activeSets.reserve(asyncState_->activeMappedSourceSets.size());
        for (auto& entry : asyncState_->activeMappedSourceSets) {
            activeSets.emplace_back(entry.first, std::move(entry.second));
            asyncState_->inFlightRequests.erase(entry.first);
        }
        abandonedFallbackSources.reserve(
            asyncState_->pendingSourceFallbacks.size());
        for (const PendingSourceFallback& fallback :
             asyncState_->pendingSourceFallbacks) {
            abandonedFallbackSources.push_back(fallback.originalKey);
        }
        asyncState_->activeMappedSourceSets.clear();
        asyncState_->activeMappedSourceSetOrder.clear();
        asyncState_->pendingSourceFallbacks.clear();
    }

    if (sourceAssetDepot_) {
        for (const TileKey& key : abandonedFallbackSources) {
            sourceAssetDepot_->abandonInFlightSource(key);
        }
    }

    for (const auto& [cacheKey, sourceSet] : activeSets) {
        if (sourceSet) {
            sourceSet->markAbandoned();
            sourceSet->releaseThrottleSlotOnce();
        }
        auto tileIt = tiles_.find(cacheKey);
        if (tileIt != tiles_.end() && tileIt->second) {
            tileIt->second->setMoreDetailAvailable(
                RasterOverlayTile::MoreDetailAvailable::No);
            tileIt->second->setState(RasterOverlayTile::LoadState::Failed);
        }
    }
    if (!activeSets.empty()) {
        asyncState_->revision.fetch_add(1, std::memory_order_relaxed);
        asyncState_->resolveDestructionIfComplete();
    }
}

void RasterOverlayTileProvider::discardPendingUploadsForMissingTiles() {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    auto& pendingUploads = asyncState_->pendingUploads;
    pendingUploads.erase(
        std::remove_if(
            pendingUploads.begin(),
            pendingUploads.end(),
            [this, state = asyncState_.get()](const PendingUpload& upload) {
                if (tiles_.find(upload.cacheKey) != tiles_.end()) {
                    return false;
                }
                state->pendingUploadBytes = std::max<int64_t>(
                    0,
                    state->pendingUploadBytes - pendingUploadSizeBytes(upload));
                enforceSourceDepotBudgetLocked(*state);
                return true;
            }),
        pendingUploads.end());
}

int RasterOverlayTileProvider::getMaximumLevel() const {
    const int configuredMaximumLevel =
        maximumLevel_ > 0
            ? maximumLevel_
            : std::min(scheme_.maxZoom(), provider_.maxZoom());
    return std::min({scheme_.maxZoom(), provider_.maxZoom(),
                     configuredMaximumLevel});
}

std::string RasterOverlayTileProvider::tileCacheKey(const TileKey& key) const {
    return key.schemeId.str() + "/" + std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" + std::to_string(key.y);
}

RasterOverlayTileProvider::TilePtr RasterOverlayTileProvider::getPlaceholderTile() {
    if (!placeholderTile_) {
        placeholderTile_ = std::make_shared<RasterOverlayTile>(*this);
    }
    return placeholderTile_;
}

RasterOverlayTileProvider::TilePtr RasterOverlayTileProvider::getTile(
    const TileKey& key) {
    // cesium-native: return placeholder if provider is not yet ready
    if (!ready_) {
        return getPlaceholderTile();
    }

    if (key.z < getMinimumLevel() || key.z > getMaximumLevel()) return nullptr;
    if (!provider_.supportsTile(key)) return nullptr;
    Rectangle geographicBounds = scheme_.tileToRectangle(key);
    const Rectangle effectiveCoverage =
        effectiveCoverageRectangle(scheme_, coverageRectangle_);
    if (!geographicBounds.computeIntersection(effectiveCoverage)) return nullptr;
    Rectangle bounds =
        projectGeographicToProvider(geographicBounds, projection_);

    std::string ck = tileCacheKey(key);
    auto it = tiles_.find(ck);
    if (it != tiles_.end()) {
        it->second->lastUsedFrame = frameNumber_;
        return it->second;
    }

    // Create new tile in Unloaded state
    auto tile = std::make_shared<RasterOverlayTile>(*this, key, bounds, ck);
    tile->setMaxZoom(getMaximumLevel());
    tile->lastUsedFrame = frameNumber_;
    tiles_[ck] = tile;
    return tile;
}

RasterOverlayTileProvider::RasterTileMapping
RasterOverlayTileProvider::mapRasterTilesToGeometryTile(
    const Rectangle& providerGeometryBounds,
    double targetScreenPixelsX,
    double targetScreenPixelsY) {
    if (!ready_) {
        return {getPlaceholderTile(), false, {}};
    }

    const Rectangle geometryBounds =
        unprojectProviderToGeographic(providerGeometryBounds, projection_);
    const Rectangle effectiveCoverage =
        effectiveCoverageRectangle(scheme_, coverageRectangle_);
    const std::optional<Rectangle> sourceBounds =
        mapGeometryBoundsToImageryCoverage(
            geometryBounds,
            effectiveCoverage,
            shouldClampOutsideCoverage(owner_));
    if (!sourceBounds) {
        return {nullptr, false, {}};
    }

    QuadtreeSourcePlan sourcePlan = buildQuadtreeSourcePlan(
        scheme_,
        provider_,
        textureUploader_.get(),
        geometryBounds,
        *sourceBounds,
        targetScreenPixelsX,
        targetScreenPixelsY,
        maximumScreenSpaceError_,
        maximumTextureSize_,
        getMinimumLevel(),
        getMaximumLevel());

    if (sourcePlan.empty()) {
        return {getPlaceholderTile(), false, {}};
    }

    RasterSourceTileMapping sourceTiles{
        sourcePlan.sourceZoom,
        *sourceBounds,
        sourcePlan.sourceKeys,
        sourcePlan.minX,
        sourcePlan.minY,
        sourcePlan.maxX,
        sourcePlan.maxY};

    if (sourcePlan.sourceKeys.size() == 1) {
        const TileKey& sourceKey = sourcePlan.sourceKeys.front();
        const Rectangle sourceTileBounds = scheme_.tileToRectangle(sourceKey);
        if (rectanglesEqualForDirectRasterTile(geometryBounds,
                                               sourceTileBounds) &&
            rectanglesEqualForDirectRasterTile(*sourceBounds,
                                               sourceTileBounds)) {
            return {getTile(sourceKey), true, std::move(sourceTiles)};
        }
    }

    const std::string ck = mappedRasterTileCacheKey(
        scheme_,
        providerGeometryBounds,
        *sourceBounds,
        sourceTiles,
        mappedRasterTileEpoch_);
    auto existing = tiles_.find(ck);
    if (existing != tiles_.end()) {
        existing->second->lastUsedFrame = frameNumber_;
        existing->second->setMappedSourceList(
            sourcePlan.sourceZoom,
            *sourceBounds,
            sourcePlan.sourceKeys,
            sourcePlan.minX,
            sourcePlan.minY,
            sourcePlan.maxX,
            sourcePlan.maxY);
        existing->second->setTargetScreenPixels(
            targetScreenPixelsX,
            targetScreenPixelsY);
        return {existing->second, false, std::move(sourceTiles)};
    }

    const double centerLng =
        geometryBounds.west() + geometryBounds.width() * 0.5;
    const double centerLat =
        geometryBounds.south() + geometryBounds.height() * 0.5;
    TileKey representativeKey = scheme_.positionToTile(
        centerLng, centerLat, sourcePlan.sourceZoom);

    auto tile = std::make_shared<RasterOverlayTile>(
        *this, representativeKey, providerGeometryBounds, ck);
    tile->setMaxZoom(getMaximumLevel());
    tile->setMappedSourceList(
        sourcePlan.sourceZoom,
        *sourceBounds,
        sourcePlan.sourceKeys,
        sourcePlan.minX,
        sourcePlan.minY,
        sourcePlan.maxX,
        sourcePlan.maxY);
    tile->setTargetScreenPixels(targetScreenPixelsX, targetScreenPixelsY);
    tile->lastUsedFrame = frameNumber_;
    tiles_[ck] = tile;
    return {tile, false, std::move(sourceTiles)};
}

RasterOverlayTileProvider::TilePtr RasterOverlayTileProvider::resolveTile(
    const Rectangle& bounds,
    int desiredZoom) {
    // cesium-native: find the best tile covering the bounds at ≤ desiredZoom.
    // Tries desiredZoom first, then walks up the tree.
    const double centerLng = (bounds.west() + bounds.east()) * 0.5;
    const double centerLat = (bounds.south() + bounds.north()) * 0.5;

    for (int z = std::min(desiredZoom, getMaximumLevel());
         z >= getMinimumLevel();
         --z) {
        TileKey key = scheme_.positionToTile(centerLng, centerLat, z);
        TilePtr tile = getTile(key);

        // Loaded-without-texture tiles are cover-ready, not drawable fallback
        // sources. Surface raster binding requires a real texture.
        if (tile &&
            tile->getState() >= RasterOverlayTile::LoadState::Loaded &&
            tile->getTexture()) {
            return tile;
        }
    }
    return nullptr;
}

ProviderRequestDiagnostics
RasterOverlayTileProvider::requestDiagnostics() const {
    ProviderRequestDiagnostics diag = provider_.requestDiagnostics();
    diag.externalResourceRequestsStarted +=
        asyncState_->rasterSourceRequestsStarted.load(
            std::memory_order_relaxed);
    diag.externalResourceRequestsCompleted +=
        asyncState_->rasterSourceRequestsCompleted.load(
            std::memory_order_relaxed);
    diag.externalResourceRequestsFailed +=
        asyncState_->rasterSourceRequestsFailed.load(
            std::memory_order_relaxed);
    diag.activeExternalResourceBlockingRequests +=
        static_cast<int>(
            asyncState_->activeRasterSourceRequests.load(
                std::memory_order_relaxed));
    diag.peakExternalResourceBlockingRequests =
        std::max(
            diag.peakExternalResourceBlockingRequests,
            static_cast<int>(
                asyncState_->peakRasterSourceRequests.load(
                    std::memory_order_relaxed)));
    diag.peakExternalResourceBlockingRequests =
        std::max(diag.peakExternalResourceBlockingRequests,
                 diag.activeExternalResourceBlockingRequests);
    return diag;
}

Texture* RasterOverlayTileProvider::getTexture(const TileKey& key) const {
    std::string ck = tileCacheKey(key);
    auto it = tiles_.find(ck);
    if (it != tiles_.end() && it->second->getTexture()) {
        return it->second->getTexture();
    }
    return nullptr;
}

int RasterOverlayTileProvider::getThrottledTilesCurrentlyLoading() const {
    return static_cast<int>(
        asyncState_->activeRasterTileLoads.load(std::memory_order_relaxed));
}

int RasterOverlayTileProvider::getPendingUploadCount() const {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    return static_cast<int>(asyncState_->pendingUploads.size());
}

int64_t RasterOverlayTileProvider::getPendingUploadBytes() const {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    return asyncState_->pendingUploadBytes;
}

int64_t RasterOverlayTileProvider::getPeakPendingUploadBytes() const {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    return asyncState_->peakPendingUploadBytes;
}

bool RasterOverlayTileProvider::pendingUploadBackpressureActive() const {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    if (asyncState_->subTileCacheBytes <= 0) {
        return false;
    }
    return asyncState_->pendingUploadBytes >= asyncState_->subTileCacheBytes;
}

bool RasterOverlayTileProvider::loadTile(RasterOverlayTile& tile,
                                         FrameResourceBudget* budget) {
    if (tile.isMappedRasterTile()) {
        return loadMappedRasterTile(tile, budget);
    }

    // cesium-native ActivatedRasterOverlay::doLoad: only Unloaded tiles start
    // a load. Loading/Loaded/Done/Failed/Placeholder are terminal or already
    // in progress from this entry point.
    auto loadState = tile.getState();
    switch (loadState) {
        case RasterOverlayTile::LoadState::Unloaded:
            break;  // OK to load
        case RasterOverlayTile::LoadState::Loading:
        case RasterOverlayTile::LoadState::Loaded:
        case RasterOverlayTile::LoadState::Done:
        case RasterOverlayTile::LoadState::Failed:
        case RasterOverlayTile::LoadState::Placeholder:
            return true;  // Already in progress or complete
    }

    const TileKey& key = tile.getTileID();
    const Rectangle outputBounds = tile.getRectangle();
    const Rectangle targetBounds =
        unprojectProviderToGeographic(outputBounds, projection_);
    return loadSourceTileList(
        tile,
        RasterSourceTileMapping{
            key.z,
            targetBounds,
            {key},
            key.x,
            key.y,
            key.x,
            key.y},
        targetBounds,
        tileCacheKey(key),
        budget);
}

bool RasterOverlayTileProvider::loadTileThrottled(RasterOverlayTile& tile,
                                                  FrameResourceBudget* budget) {
    // cesium-native: loadTileThrottled only starts Unloaded tiles. Once a
    // mapped raster tile is Loading, it may still have unissued source futures
    // waiting for raster request budget. Keep pumping those shared source
    // assets so large rectangle compositions converge over multiple frames.
    if (tile.isMappedRasterTile() &&
        tile.getState() == RasterOverlayTile::LoadState::Loading) {
        pumpLoadingMappedRasterTile(tile, budget);
        return true;
    }
    if (tile.getState() != RasterOverlayTile::LoadState::Unloaded) {
        return true;
    }
    if (pendingUploadBackpressureActive()) {
        return false;
    }

    const bool canJoinExistingMappedSources =
        tile.isMappedRasterTile() &&
        !mappedTileWouldIssueNewSourceRequests(tile);
    if (!canJoinExistingMappedSources &&
        getThrottledTilesCurrentlyLoading() >= maximumSimultaneousTileLoads) {
        return false;  // Throttled
    }

    return loadTile(tile, budget);
}

bool RasterOverlayTileProvider::loadMappedRasterTile(
    RasterOverlayTile& tile,
    FrameResourceBudget* budget) {
    auto loadState = tile.getState();
    switch (loadState) {
        case RasterOverlayTile::LoadState::Unloaded:
            break;
        case RasterOverlayTile::LoadState::Loading:
            pumpLoadingMappedRasterTile(tile, budget);
            return true;
        case RasterOverlayTile::LoadState::Loaded:
        case RasterOverlayTile::LoadState::Done:
        case RasterOverlayTile::LoadState::Failed:
        case RasterOverlayTile::LoadState::Placeholder:
            return true;
    }

    const std::string ck = tile.getCacheKey();
    if (ck.empty()) return false;

    const Rectangle outputBounds = tile.getRectangle();
    const Rectangle targetBounds =
        unprojectProviderToGeographic(outputBounds, projection_);
    RasterSourceTileMapping sourceTiles;
    const Rectangle effectiveCoverage =
        effectiveCoverageRectangle(scheme_, coverageRectangle_);
    if (tile.hasMappedSourceList() &&
        tile.getMappedSourceBounds().computeIntersection(
            effectiveCoverage)) {
        sourceTiles.sourceZoom = tile.getMappedSourceZoom();
        sourceTiles.sourceBounds = tile.getMappedSourceBounds();
        sourceTiles.sourceKeys = tile.getMappedSourceKeys();
        sourceTiles.minX = tile.getMappedSourceMinX();
        sourceTiles.minY = tile.getMappedSourceMinY();
        sourceTiles.maxX = tile.getMappedSourceMaxX();
        sourceTiles.maxY = tile.getMappedSourceMaxY();
    } else {
        const std::optional<Rectangle> mappedSourceBounds =
            mapGeometryBoundsToImageryCoverage(
                targetBounds,
                effectiveCoverage,
                shouldClampOutsideCoverage(owner_));
        if (!mappedSourceBounds) {
            logAndroidRasterPipeline("coverage-miss", ck, 0, 0);
            tile.setMoreDetailAvailable(
                RasterOverlayTile::MoreDetailAvailable::No);
            tile.setState(RasterOverlayTile::LoadState::Failed);
            return false;
        }
        QuadtreeSourcePlan sourcePlan = buildQuadtreeSourcePlan(
            scheme_,
            provider_,
            textureUploader_.get(),
            targetBounds,
            *mappedSourceBounds,
            tile.getTargetScreenPixelsX(),
            tile.getTargetScreenPixelsY(),
            maximumScreenSpaceError_,
            maximumTextureSize_,
            getMinimumLevel(),
            getMaximumLevel());
        tile.setMappedSourceList(
            sourcePlan.sourceZoom,
            *mappedSourceBounds,
            sourcePlan.sourceKeys,
            sourcePlan.minX,
            sourcePlan.minY,
            sourcePlan.maxX,
            sourcePlan.maxY);
        sourceTiles = RasterSourceTileMapping{
            sourcePlan.sourceZoom,
            *mappedSourceBounds,
            sourcePlan.sourceKeys,
            sourcePlan.minX,
            sourcePlan.minY,
            sourcePlan.maxX,
            sourcePlan.maxY};
    }

    if (sourceTiles.empty()) {
        logAndroidRasterPipeline("empty-plan", ck, 0, sourceTiles.sourceZoom);
        tile.setMoreDetailAvailable(RasterOverlayTile::MoreDetailAvailable::No);
        tile.setState(RasterOverlayTile::LoadState::Failed);
        return false;
    }
    logAndroidRasterPipeline(
        "start",
        ck,
        static_cast<int>(sourceTiles.sourceKeys.size()),
        sourceTiles.sourceZoom);
    return loadSourceTileList(
        tile,
        std::move(sourceTiles),
        targetBounds,
        ck,
        budget);
}

bool RasterOverlayTileProvider::pumpLoadingMappedRasterTile(
    RasterOverlayTile& tile,
    FrameResourceBudget* budget) {
    if (!tile.isMappedRasterTile() ||
        tile.getState() != RasterOverlayTile::LoadState::Loading) {
        return false;
    }

    std::shared_ptr<MappedSourceImageSet> sourceSet;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        auto it = asyncState_->activeMappedSourceSets.find(
            tile.getCacheKey());
        if (it != asyncState_->activeMappedSourceSets.end()) {
            sourceSet = it->second;
        }
    }
    if (sourceSet && sourceSet->hasUnissuedSources()) {
        issueMappedSourceImageSet(sourceSet, budget);
    }
    return true;
}

bool RasterOverlayTileProvider::loadSourceTileList(
    RasterOverlayTile& tile,
    RasterSourceTileMapping sourceTiles,
    const Rectangle& targetBounds,
    const std::string& cacheKey,
    FrameResourceBudget* budget) {
    const Rectangle composeBounds =
        sourceTiles.sourceBounds.isEmpty() ? targetBounds
                                           : sourceTiles.sourceBounds;
    return loadSourceImageSet(
        tile,
        std::move(sourceTiles),
        composeBounds,
        cacheKey,
        budget);
}

bool RasterOverlayTileProvider::loadSourceImageSet(
    RasterOverlayTile& tile,
    RasterSourceTileMapping sourceTiles,
    const Rectangle& targetBounds,
    const std::string& cacheKey,
    FrameResourceBudget* budget) {
    if (cacheKey.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        if (asyncState_->inFlightRequests.count(cacheKey)) return true;
    }

    const int estimatedNewSourceRequests =
        estimateNewSourceRequestsForSourceKeys(sourceTiles.sourceKeys);
    const int availableSourceRequestSlots = availableRasterRequestSlots(
        budget,
        asyncState_->activeRasterSourceRequests.load(
            std::memory_order_relaxed));
    // cesium-native SharedAssetDepot::getOrCreate returns an existing pending
    // or loaded asset without starting transport work. Preserve that budget
    // behavior for both mapped and direct raster loads.
    const bool hasReusableSharedSource =
        estimatedNewSourceRequests <
            static_cast<int>(sourceTiles.sourceKeys.size());
    if (estimatedNewSourceRequests > 0 &&
        availableSourceRequestSlots <= 0 &&
        !hasReusableSharedSource) {
        return false;
    }
    if (!tile.isMappedRasterTile() && estimatedNewSourceRequests > 0 &&
        !hasRasterInflightCapacity(
            budget,
            asyncState_->activeRasterSourceRequests.load(
                std::memory_order_relaxed),
            estimatedNewSourceRequests)) {
        return false;
    }

    tile.setState(RasterOverlayTile::LoadState::Loading);
    asyncState_->activeRasterTileLoads.fetch_add(
        1,
        std::memory_order_relaxed);
    // 本次加载名额的唯一释放令牌：完成回调与 abandon/析构共用，谁先
    // exchange 谁递减（见 releaseRasterThrottleSlotOnce）
    auto throttleSlotReleased = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        asyncState_->inFlightRequests.insert(cacheKey);
    }

    std::shared_ptr<ProviderAsyncState> state = asyncState_;
    std::weak_ptr<RasterOverlayTile> tileWeak;
    auto tileIt = tiles_.find(cacheKey);
    if (tileIt != tiles_.end()) {
        tileWeak = tileIt->second;
    }
    if (!sourceAssetDepot_) {
        refreshSourceAssetDepot();
    }
    uint64_t requestSourceDepotEpoch = 0;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        requestSourceDepotEpoch = asyncState_->sourceTileDepotEpoch;
    }
    const bool returnEmptyForAncestorOnly = true;
    auto sourceSet = std::make_shared<MappedSourceImageSet>(
        scheme_,
        state,
        throttleSlotReleased,
        sourceAssetDepot_,
        std::move(sourceTiles),
        targetBounds,
        projection_,
        getMaximumLevel(),
        returnEmptyForAncestorOnly,
        !tile.isMappedRasterTile(),
        [state, throttleSlotReleased, cacheKey, tileWeak,
         requestSourceDepotEpoch](
            std::unique_ptr<DecodedImage> composed,
            std::shared_ptr<const DecodedImage> sharedImage,
            Rectangle rectangle,
            RasterOverlayTile::MoreDetailAvailable moreDetailAvailable,
            std::vector<std::string> diagnostics,
            std::vector<std::string> credits) {
            std::lock_guard<std::mutex> providerLock(state->mutex);
            state->inFlightRequests.erase(cacheKey);
            state->activeMappedSourceSets.erase(cacheKey);
            if (!state->alive.load(std::memory_order_acquire)) {
                releaseRasterThrottleSlotOnce(
                    *throttleSlotReleased,
                    state->activeRasterTileLoads);
                state->resolveDestructionIfComplete();
                return;
            }
            if (state->sourceTileDepotEpoch != requestSourceDepotEpoch) {
                if (auto tile = tileWeak.lock()) {
                    tile->setMoreDetailAvailable(
                        RasterOverlayTile::MoreDetailAvailable::No);
                    tile->setState(RasterOverlayTile::LoadState::Failed);
                }
                releaseRasterThrottleSlotOnce(
                    *throttleSlotReleased,
                    state->activeRasterTileLoads);
                state->resolveDestructionIfComplete();
                state->revision.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            logAndroidRasterPipeline("composed", cacheKey, 0, 0);
            PendingUpload pendingUpload{
                cacheKey,
                std::move(composed),
                std::move(sharedImage),
                rectangle,
                moreDetailAvailable,
                std::move(diagnostics),
                std::move(credits)};
            state->pendingUploadBytes += pendingUploadSizeBytes(pendingUpload);
            trackPeakBytes(
                state->pendingUploadBytes,
                state->peakPendingUploadBytes);
            enforceSourceDepotBudgetLocked(*state);
            state->pendingUploads.push_back(
                std::move(pendingUpload));
            // cesium _totalTilesCurrentlyLoading 语义：节流名额在加载
            // （下载+合成）完成时释放；GPU 上传属主线程 prepare 阶段，
            // 由 RasterTextureUpload lane 单独限速。此前名额持有到上传
            // 消费，交互期上传被 defer 时节流被积压占满（真机 54/20），
            // 新加载全部被卡。
            releaseRasterThrottleSlotOnce(
                *throttleSlotReleased,
                state->activeRasterTileLoads);
            state->resolveDestructionIfComplete();
        },
        [state, throttleSlotReleased, cacheKey, tileWeak,
         requestSourceDepotEpoch](
            std::vector<std::string> diagnostics) {
            std::lock_guard<std::mutex> providerLock(state->mutex);
            state->inFlightRequests.erase(cacheKey);
            state->activeMappedSourceSets.erase(cacheKey);
            if (!state->alive.load(std::memory_order_acquire)) {
                releaseRasterThrottleSlotOnce(
                    *throttleSlotReleased,
                    state->activeRasterTileLoads);
                state->resolveDestructionIfComplete();
                return;
            }
            if (state->sourceTileDepotEpoch != requestSourceDepotEpoch) {
                if (auto tile = tileWeak.lock()) {
                    tile->setMoreDetailAvailable(
                        RasterOverlayTile::MoreDetailAvailable::No);
                    tile->setState(RasterOverlayTile::LoadState::Failed);
                }
                releaseRasterThrottleSlotOnce(
                    *throttleSlotReleased,
                    state->activeRasterTileLoads);
                state->resolveDestructionIfComplete();
                state->revision.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            logAndroidRasterPipeline("compose-failed", cacheKey, 0, 0);
            if (auto tile = tileWeak.lock()) {
                const bool transientFailure =
                    isTransientRasterSourceFailure(diagnostics);
                tile->setLoadDiagnostics(std::move(diagnostics));
                tile->setMoreDetailAvailable(
                    RasterOverlayTile::MoreDetailAvailable::No);
                tile->setState(
                    transientFailure
                        ? RasterOverlayTile::LoadState::Unloaded
                        : RasterOverlayTile::LoadState::Failed);
            }
            releaseRasterThrottleSlotOnce(
                *throttleSlotReleased,
                state->activeRasterTileLoads);
            state->resolveDestructionIfComplete();
            state->revision.fetch_add(1, std::memory_order_relaxed);
        });

    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        const bool inserted =
            asyncState_->activeMappedSourceSets
                .emplace(cacheKey, sourceSet)
                .second;
        if (!inserted) {
            asyncState_->activeMappedSourceSets[cacheKey] = sourceSet;
        } else {
            asyncState_->activeMappedSourceSetOrder.push_back(cacheKey);
        }
    }

    issueMappedSourceImageSet(sourceSet, budget);

    return true;
}

int RasterOverlayTileProvider::issueMappedSourceImageSet(
    const std::shared_ptr<MappedSourceImageSet>& sourceSet,
    FrameResourceBudget* budget) {
    if (pendingUploadBackpressureActive()) {
        return 0;
    }
    if (!sourceSet || sourceSet->isComplete()) {
        return 0;
    }
    std::shared_ptr<ProviderAsyncState> state = asyncState_;
    auto onSourceIssued = [state]() {
        state->rasterSourceRequestsStarted.fetch_add(
            1,
            std::memory_order_relaxed);
        const uint32_t active =
            state->activeRasterSourceRequests.fetch_add(
                1,
                std::memory_order_relaxed) +
            1;
        uint32_t peak = state->peakRasterSourceRequests.load(
            std::memory_order_relaxed);
        while (active > peak &&
               !state->peakRasterSourceRequests.compare_exchange_weak(
                   peak,
                   active,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
    };
    auto onSourceFinished = [state]() {
        state->rasterSourceRequestsCompleted.fetch_add(
            1,
            std::memory_order_relaxed);
        uint32_t current = state->activeRasterSourceRequests.load(
            std::memory_order_relaxed);
        while (current > 0 &&
               !state->activeRasterSourceRequests.compare_exchange_weak(
                   current,
                   current - 1,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
        state->resolveDestructionIfComplete();
    };
    auto onSourceFailed = [state]() {
        state->rasterSourceRequestsFailed.fetch_add(
            1,
            std::memory_order_relaxed);
    };

    const int maxToIssue = availableRasterRequestSlots(
        budget,
        state->activeRasterSourceRequests.load(
            std::memory_order_relaxed));
    const int newlyIssued =
        sourceSet->issueSome(
            maxToIssue,
            onSourceIssued,
            onSourceFinished,
            onSourceFailed);
    if (budget && newlyIssued > 0) {
        budget->tryIssue(
            FrameResourceLane::RasterRequest,
            FrameResourcePriority::Normal,
            newlyIssued);
    }
    return newlyIssued;
}

int RasterOverlayTileProvider::estimateNewSourceRequestsForSourceKeys(
    const std::vector<TileKey>& sourceKeys) const {
    int estimated = 0;
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    for (const TileKey& sourceKey : sourceKeys) {
        const std::string sourceKeyString =
            sourceCacheKey(
                asyncState_->sourceTileDepotEpoch,
                sourceKey);
        auto cached =
            asyncState_->sourceTileDepotCache.find(sourceKeyString);
        if (cached != asyncState_->sourceTileDepotCache.end() &&
            (cached->second.image || cached->second.terminalFailure)) {
            continue;
        }
        if (asyncState_->sourceTileDepotInFlight.count(sourceKeyString) > 0) {
            continue;
        }
        ++estimated;
    }
    return estimated;
}

bool RasterOverlayTileProvider::mappedTileWouldIssueNewSourceRequests(
    const RasterOverlayTile& tile) const {
    if (!tile.isMappedRasterTile() || !tile.hasMappedSourceList()) {
        return true;
    }
    return estimateNewSourceRequestsForSourceKeys(
               tile.getMappedSourceKeys()) > 0;
}

int RasterOverlayTileProvider::issuePendingSourceFallbacks(
    FrameResourceBudget* budget) {
    if (pendingUploadBackpressureActive()) {
        return 0;
    }
    int issued = 0;
    while (true) {
        if (budget) {
            const int remainingSlots = availableRasterRequestSlots(
                budget,
                asyncState_->activeRasterSourceRequests.load(
                    std::memory_order_relaxed));
            if (remainingSlots <= 0) {
                break;
            }
        }

        PendingSourceFallback fallback;
        {
            std::lock_guard<std::mutex> lock(asyncState_->mutex);
            if (asyncState_->pendingSourceFallbacks.empty()) {
                break;
            }
            fallback =
                std::move(asyncState_->pendingSourceFallbacks.front());
            asyncState_->pendingSourceFallbacks.pop_front();
        }

        const int newlyIssued = fallback.issue ? fallback.issue() : 0;
        if (newlyIssued > 0) {
            issued += newlyIssued;
            if (budget) {
                budget->tryIssue(
                    FrameResourceLane::RasterRequest,
                    FrameResourcePriority::Normal,
                    newlyIssued);
            }
        }
    }
    return issued;
}

int RasterOverlayTileProvider::issueActiveMappedSourceImageSets(
    FrameResourceBudget* budget) {
    int issued = issuePendingSourceFallbacks(budget);
    std::vector<std::shared_ptr<MappedSourceImageSet>> activeSets;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        activeSets.reserve(asyncState_->activeMappedSourceSetOrder.size());
        std::deque<std::string> compactedOrder;
        for (const std::string& cacheKey :
             asyncState_->activeMappedSourceSetOrder) {
            auto it = asyncState_->activeMappedSourceSets.find(cacheKey);
            if (it == asyncState_->activeMappedSourceSets.end() ||
                !it->second) {
                continue;
            }
            compactedOrder.push_back(cacheKey);
            activeSets.push_back(it->second);
        }
        asyncState_->activeMappedSourceSetOrder.swap(compactedOrder);
    }

    for (const auto& sourceSet : activeSets) {
        if (!sourceSet || !sourceSet->hasUnissuedSources()) {
            continue;
        }
        issued += issueMappedSourceImageSet(sourceSet, budget);
    }
    return issued;
}

int RasterOverlayTileProvider::processPendingUploads(
    bool interactionActive,
    FrameResourceBudget* budget) {
    // cesium-native: process completed HTTP responses on main thread.
    // Create GPU textures and mark tiles as Loaded.
    FrameResourceBudget localBudget;
    if (!budget) {
        FrameResourceBudgetConfig config;
        config.maxRasterUploadsPerFrame =
            static_cast<uint32_t>(kDefaultMaximumRasterUploadsPerFrame);
        config.interactionActive = interactionActive;
        config.smoothingActive = interactionActive;
        localBudget.beginFrame(frameNumber_, config);
        budget = &localBudget;
    }

    issueActiveMappedSourceImageSets(budget);

    int processed = 0;
    while (true) {
        PendingUpload upload;
        {
            std::lock_guard<std::mutex> lock(asyncState_->mutex);
            // Mapped raster tiles can be 512x512+ and state finalization
            // runs on the main thread. Take one upload at a time so elapsed
            // upload cost can stop the next item in the same frame.
            if (asyncState_->pendingUploads.empty()) {
                break;
            }
            if (!budget->tryFinalize(FrameResourceLane::RasterTextureUpload,
                                     FrameResourcePriority::Normal)) {
                break;
            }
            auto selected = asyncState_->pendingUploads.begin();
            if (interactionActive) {
                selected = std::find_if(
                    asyncState_->pendingUploads.begin(),
                    asyncState_->pendingUploads.end(),
                    [](const PendingUpload& candidate) {
                        const DecodedImage* candidateImage = candidate.image
                            ? candidate.image.get()
                            : candidate.sharedImage.get();
                        return uploadAllowedDuringInteraction(
                            candidate.cacheKey,
                            candidateImage);
                    });
                if (selected == asyncState_->pendingUploads.end()) {
                    break;
                }
            }
            upload = std::move(*selected);
            asyncState_->pendingUploadBytes = std::max<int64_t>(
                0,
                asyncState_->pendingUploadBytes -
                    pendingUploadSizeBytes(upload));
            enforceSourceDepotBudgetLocked(*asyncState_);
            asyncState_->pendingUploads.erase(selected);
        }

        std::vector<TilePtr> targetTiles;
        if (auto it = tiles_.find(upload.cacheKey); it != tiles_.end()) {
            targetTiles.push_back(it->second);
        }
        targetTiles.erase(
            std::remove_if(
                targetTiles.begin(),
                targetTiles.end(),
                [this](const TilePtr& target) {
                    return !target ||
                           (target->isMappedRasterTile() &&
                            !ownsCurrentTile(*target));
                }),
            targetTiles.end());
        if (targetTiles.empty()) {
            // 节流名额已在加载完成入队时释放，这里只丢弃孤儿上传
            continue;
        }

        const DecodedImage* uploadImage = upload.image
            ? upload.image.get()
            : upload.sharedImage.get();
        const bool emptyImage =
            uploadImage &&
            uploadImage->width == 0 &&
            uploadImage->height == 0 &&
            uploadImage->channels == 0 &&
            uploadImage->pixels.empty();
        if (emptyImage) {
            for (const TilePtr& target : targetTiles) {
                target->setLoadDiagnostics(upload.diagnostics);
                target->setCredits(upload.credits);
                target->setMoreDetailAvailable(
                    RasterOverlayTile::MoreDetailAvailable::No);
                target->setRectangle(upload.rectangle);
                target->markLoadedWithoutTexture();
            }
            asyncState_->revision.fetch_add(1, std::memory_order_relaxed);
            ++processed;
            continue;
        }

        if (!uploadImage || !isDecodedImageUploadable(*uploadImage)) {
            for (const TilePtr& target : targetTiles) {
                target->setLoadDiagnostics(upload.diagnostics);
                target->setCredits(upload.credits);
                target->setMoreDetailAvailable(
                    RasterOverlayTile::MoreDetailAvailable::No);
                target->setState(RasterOverlayTile::LoadState::Failed);
            }
            asyncState_->revision.fetch_add(1, std::memory_order_relaxed);
            ++processed;
            continue;
        }

        const double uploadStartMs = perf::nowMs();
        double uploadMs = 0.0;
        for (const TilePtr& target : targetTiles) {
            RasterOverlayTile& tile = *target;
            // Resource-prep upload (main-thread safe). Mapped raster images are
            // already combined at the selector's target screen-pixel density;
            // on mobile, generating mipmaps for every mapped raster image is
            // expensive main-thread work without improving the current
            // selected tile.
            const bool generateMipmaps = false;
            RasterTextureUploadOptions uploadOptions;
            uploadOptions.generateMipmaps = generateMipmaps;
            auto tex = textureUploader_
                ? textureUploader_->uploadRasterTexture(
                      *uploadImage,
                      uploadOptions)
                : nullptr;
            uploadMs = perf::nowMs() - uploadStartMs;
            if (!tex) {
                tile.setLoadDiagnostics(upload.diagnostics);
                tile.setCredits(upload.credits);
                tile.setMoreDetailAvailable(
                    RasterOverlayTile::MoreDetailAvailable::No);
                tile.setState(RasterOverlayTile::LoadState::Failed);
                continue;
            }
            const int sourceLevel =
                tile.isMappedRasterTile() ? tile.getMappedSourceZoom() : tile.getTileID().z;
            const RasterOverlayTile::MoreDetailAvailable moreDetailAvailable =
                upload.moreDetailAvailable !=
                        RasterOverlayTile::MoreDetailAvailable::Unknown
                    ? upload.moreDetailAvailable
                    : (sourceLevel < tile.getMaxZoom()
                           ? RasterOverlayTile::MoreDetailAvailable::Yes
                           : RasterOverlayTile::MoreDetailAvailable::No);
            tile.setMoreDetailAvailable(moreDetailAvailable);
            tile.setLoadDiagnostics(upload.diagnostics);
            tile.setCredits(upload.credits);
            tile.setRectangle(upload.rectangle);
            // cesium-native: transfer texture ownership to the tile.
            // The tile owns its texture; no external cache needed.
            tile.setTexture(std::move(tex));
            platformLog(LogLevel::Info, "RasterOverlayTileProvider",
                "Tile loaded: %d/%d/%d", tile.getTileID().z,
                tile.getTileID().x, tile.getTileID().y);
            if (uploadMs >= 8.0 ||
                uploadImage->width > 1024 ||
                uploadImage->height > 1024) {
                platformLog(LogLevel::Info, "RasterOverlayTileProvider",
                    "upload %.2fms size=%dx%d mapped=%d mipmap=%d cache=%s",
                    uploadMs,
                    uploadImage->width,
                    uploadImage->height,
                    tile.isMappedRasterTile() ? 1 : 0,
                    generateMipmaps ? 1 : 0,
                    tile.getCacheKey().c_str());
            }
        }
        budget->recordElapsed(FrameResourceLane::RasterTextureUpload, uploadMs);
        asyncState_->revision.fetch_add(1, std::memory_order_relaxed);
        ++processed;
    }
    return processed;
}

bool RasterOverlayTileProvider::hasPendingWork() const {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    return !asyncState_->pendingUploads.empty() ||
           !asyncState_->inFlightRequests.empty() ||
           !asyncState_->activeMappedSourceSets.empty() ||
           !asyncState_->pendingSourceFallbacks.empty() ||
           !asyncState_->sourceTileDepotInFlight.empty() ||
           asyncState_->activeRasterComposeTasks.load(
               std::memory_order_relaxed) > 0 ||
           asyncState_->activeRasterSourceRequests.load(
               std::memory_order_relaxed) > 0;
}

void RasterOverlayTileProvider::markUsed(const std::string& cacheKey) {
    auto it = tiles_.find(cacheKey);
    if (it != tiles_.end()) {
        it->second->lastUsedFrame = frameNumber_;
    }
}

void RasterOverlayTileProvider::markUsed(const TileKey& key) {
    markUsed(tileCacheKey(key));
}

void RasterOverlayTileProvider::markUsed(const RasterOverlayTile& tile) {
    if (!tile.getCacheKey().empty()) {
        markUsed(tile.getCacheKey());
    } else {
        markUsed(tile.getTileID());
    }
}

bool RasterOverlayTileProvider::ownsCurrentTile(
    const RasterOverlayTile& tile) const {
    if (tile.getState() == RasterOverlayTile::LoadState::Placeholder) {
        return placeholderTile_ && placeholderTile_.get() == &tile;
    }

    const std::string& cacheKey = tile.getCacheKey();
    if (!cacheKey.empty()) {
        if (isEpochMappedRasterCacheKey(cacheKey)) {
            const std::string currentEpochPrefix =
                "mapped-raster/epoch/" +
                std::to_string(mappedRasterTileEpoch_) + "/";
            if (cacheKey.rfind(currentEpochPrefix, 0) != 0) {
                return false;
            }
        }
        auto it = tiles_.find(cacheKey);
        return it != tiles_.end() && it->second.get() == &tile;
    }

    auto it = tiles_.find(tileCacheKey(tile.getTileID()));
    return it != tiles_.end() && it->second.get() == &tile;
}

void RasterOverlayTileProvider::trimUnusedTiles() {
    // Keep recently referenced tiles for a short window. cesium-native retains
    // raster tiles via intrusive references and a cache budget; this local
    // provider owns tiles directly, so immediate one-frame eviction would
    // churn active mapping handles and waste in-flight IO.
    // 一次加锁快照 in-flight/待上传 key 集合后无锁遍历。原实现每瓦片一次
    // mutex 往返 + O(待上传) 线性字符串比较（300 缓存 × 50 待上传 ≈ 1.5 万
    // 次比较/帧，P2-8）。
    std::unordered_set<std::string> busyKeys;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        busyKeys.reserve(asyncState_->inFlightRequests.size() +
                         asyncState_->pendingUploads.size());
        busyKeys.insert(asyncState_->inFlightRequests.begin(),
                        asyncState_->inFlightRequests.end());
        for (const PendingUpload& upload : asyncState_->pendingUploads) {
            busyKeys.insert(upload.cacheKey);
        }
    }
    for (auto it = tiles_.begin(); it != tiles_.end(); ) {
        RasterOverlayTile& tile = *it->second;
        const uint64_t age = frameNumber_ > tile.lastUsedFrame
            ? frameNumber_ - tile.lastUsedFrame
            : 0;
        const bool inFlight = busyKeys.count(it->first) > 0;
        const bool retainedOutsideProvider = it->second.use_count() > 1;
        if (age > kRetainedUnusedFrames && !inFlight &&
            !retainedOutsideProvider) {
            it = tiles_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace earth_engine
