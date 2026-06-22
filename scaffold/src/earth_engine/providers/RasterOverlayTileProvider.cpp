#include "RasterOverlayTileProvider.h"
#include "ImageryProvider.h"
#include "../layers/RasterOverlay.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../tiling/TileScheme.h"
#include "RasterTextureUploader.h"
#include "../renderer/RenderDevice.h"
#include "../threading/CancellationToken.h"
#include "../debug/PerfTimer.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Projection.h"
#include "../core/math/MathUtils.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {
namespace {

constexpr uint64_t kRetainedUnusedFrames = 120;
constexpr int kMaximumCombinedTextureSizeFallback = 2048;
constexpr size_t kDefaultMaximumRasterUploadsPerFrame = 20;
constexpr int kInteractionRasterUploadMaxDimension = 512;
constexpr int64_t kInteractionRasterUploadMaxPixels = 512ll * 512ll;
constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kMaxWebMercatorLat = 1.4844222297453324;
constexpr double kPixelTolerance = 0.01;

#ifdef __ANDROID__
void logAndroidRasterPipeline(const char* stage,
                              const std::string& cacheKey,
                              int sourceCount,
                              int sourceZoom) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) >= 48) {
        return;
    }
    __android_log_print(
        ANDROID_LOG_INFO,
        "RasterOverlayPipe",
        "%s cache=%s sources=%d sourceZoom=%d",
        stage,
        cacheKey.c_str(),
        sourceCount,
        sourceZoom);
}
#else
void logAndroidRasterPipeline(const char*,
                              const std::string&,
                              int,
                              int) {}
#endif

bool uploadAllowedDuringInteraction(
    const std::string& cacheKey,
    const DecodedImage* image) {
    if (!image) {
        return true;
    }
    if (cacheKey.rfind("composite/", 0) == 0) {
        return false;
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
    const double veryCloseX = std::max(1e-12, geometryBounds.width()) / 512.0;
    const double veryCloseY = std::max(1e-12, geometryBounds.height()) / 512.0;

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
        if (std::abs(northTile.south() - geometryBounds.north()) < veryCloseY &&
            range.minY < range.maxY) {
            ++range.minY;
        }

        const Rectangle southTile = scheme.tileToRectangle(
            TileKey{scheme.id(), zoom, range.maxX, range.maxY});
        if (std::abs(southTile.north() - geometryBounds.south()) < veryCloseY &&
            range.maxY > range.minY) {
            --range.maxY;
        }
    } else {
        const Rectangle southTile = scheme.tileToRectangle(
            TileKey{scheme.id(), zoom, range.minX, range.minY});
        if (std::abs(southTile.north() - geometryBounds.south()) < veryCloseY &&
            range.minY < range.maxY) {
            ++range.minY;
        }

        const Rectangle northTile = scheme.tileToRectangle(
            TileKey{scheme.id(), zoom, range.maxX, range.maxY});
        if (std::abs(northTile.south() - geometryBounds.north()) < veryCloseY &&
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

bool tryIssueRasterRequestBudget(FrameResourceBudget* budget,
                                 uint32_t currentInflight,
                                 int estimatedFanout) {
    if (estimatedFanout <= 0) {
        return true;
    }
    if (!budget) {
        return true;
    }
    const FrameResourceBudgetSnapshot snapshot = budget->snapshot();
    const bool oversizedCesiumNativeBatch =
        estimatedFanout >
            static_cast<int>(snapshot.maxRasterNetworkRequestsPerFrame) ||
        estimatedFanout >
            static_cast<int>(snapshot.maxRasterNetworkInflight);
    if (oversizedCesiumNativeBatch &&
        currentInflight == 0 &&
        snapshot.rasterNetworkRequestsIssued == 0) {
        return budget->tryIssue(
            FrameResourceLane::RasterRequest,
            FrameResourcePriority::Normal,
            1);
    }
    if (!budget->hasNetworkInflightCapacity(
            FrameResourceLane::RasterRequest,
            currentInflight,
            estimatedFanout)) {
        return false;
    }
    return budget->tryIssue(
        FrameResourceLane::RasterRequest,
        FrameResourcePriority::Normal,
        estimatedFanout);
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

    // cesium-native QuadtreeRasterOverlayTileProvider maps base imagery with
    // no geometry/provider overlap to the nearest coverage edge, allowing edge
    // texels to stretch rather than dropping the raster tile entirely.
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
    return owner == nullptr || owner->role() == RasterOverlayRole::BaseImagery;
}

bool isDecodedImageUploadable(const DecodedImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.channels <= 0) {
        return false;
    }
    const int64_t requiredBytes =
        static_cast<int64_t>(image.width) *
        static_cast<int64_t>(image.height) *
        static_cast<int64_t>(image.channels);
    return requiredBytes > 0 &&
           image.pixels.size() >= static_cast<size_t>(requiredBytes);
}

int64_t decodedImageSizeBytes(const DecodedImage& image) {
    return static_cast<int64_t>(sizeof(DecodedImage)) +
           static_cast<int64_t>(image.pixels.size());
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
        const int widthPixels =
            coverage.width() * std::max(1, provider.tileWidth());
        const int heightPixels =
            coverage.height() * std::max(1, provider.tileHeight());
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

std::string compositeTileCacheKey(const TileScheme& scheme,
                                  const Rectangle& rectangle,
                                  int sourceZoom,
                                  uint64_t epoch) {
    char bounds[256];
    std::snprintf(bounds,
                  sizeof(bounds),
                  "%.17g/%.17g/%.17g/%.17g",
                  rectangle.west(),
                  rectangle.south(),
                  rectangle.east(),
                  rectangle.north());
    return "composite/epoch/" + std::to_string(epoch) + "/" +
           scheme.id() + "/srcz/" +
           std::to_string(sourceZoom) + "/" + bounds;
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

TileKey parentTileKey(const TileKey& key) {
    return key.parent();
}

std::string sourceCacheKey(const TileKey& key) {
    return key.schemeId + "/" + std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" + std::to_string(key.y);
}

std::string sourceCacheKey(uint64_t epoch, const TileKey& key) {
    return "epoch/" + std::to_string(epoch) + "/" + sourceCacheKey(key);
}

struct LoadedSourceImage {
    TileKey key;
    Rectangle bounds;
    std::shared_ptr<const DecodedImage> image;
    std::optional<Rectangle> sourceSubset;
    RasterOverlayTile::MoreDetailAvailable moreDetailAvailable =
        RasterOverlayTile::MoreDetailAvailable::Unknown;
};

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
};

PixelRectangle computePixelRectangle(
    const DecodedImage& image,
    const Rectangle& totalRectangle,
    const Rectangle& partRectangle,
    const TileScheme& scheme) {
    const double totalProjectedHeight = projectedHeight(scheme, totalRectangle);
    const double partProjectedNorth = projectedNorth(scheme, partRectangle);
    const double partProjectedSouth = projectedSouth(scheme, partRectangle);
    const double totalProjectedNorth = projectedNorth(scheme, totalRectangle);

    int x = static_cast<int>(MathUtils::roundDown(
        image.width * (partRectangle.west() - totalRectangle.west()) /
            totalRectangle.width(),
        kPixelTolerance));
    x = std::max(0, x);

    int y = static_cast<int>(MathUtils::roundDown(
        image.height * (totalProjectedNorth - partProjectedNorth) /
            totalProjectedHeight,
        kPixelTolerance));
    y = std::max(0, y);

    int maxX = static_cast<int>(MathUtils::roundUp(
        image.width * (partRectangle.east() - totalRectangle.west()) /
            totalRectangle.width(),
        kPixelTolerance));
    maxX = std::min(maxX, image.width);

    int maxY = static_cast<int>(MathUtils::roundUp(
        image.height * (totalProjectedNorth - partProjectedSouth) /
            totalProjectedHeight,
        kPixelTolerance));
    maxY = std::min(maxY, image.height);

    return PixelRectangle{x, y, std::max(0, maxX - x), std::max(0, maxY - y)};
}

CombinedImageMeasurements measureCombinedImage(
    const TileScheme& scheme,
    const Rectangle& targetBounds,
    const std::vector<LoadedSourceImage>& sources,
    double projectedWidthPerPixel,
    double projectedHeightPerPixel,
    int maximumTextureSize) {
    std::optional<Rectangle> combinedBounds;
    for (const LoadedSourceImage& source : sources) {
        const Rectangle sourceSubset =
            source.sourceSubset.value_or(source.bounds);
        std::optional<Rectangle> intersection =
            targetBounds.computeIntersection(sourceSubset);
        if (!intersection) {
            continue;
        }

        const double projectedSouthValue = projectedSouth(scheme, *intersection);
        const double projectedNorthValue = projectedNorth(scheme, *intersection);
        const double roundedProjectedSouth =
            MathUtils::roundDown(
                projectedSouthValue / projectedHeightPerPixel,
                kPixelTolerance) *
            projectedHeightPerPixel;
        const double roundedProjectedNorth =
            MathUtils::roundUp(
                projectedNorthValue / projectedHeightPerPixel,
                kPixelTolerance) *
            projectedHeightPerPixel;

        Rectangle expanded(
            MathUtils::roundDown(
                intersection->west() / projectedWidthPerPixel,
                kPixelTolerance) *
                projectedWidthPerPixel,
            latitudeAtProjectedY(scheme, roundedProjectedSouth),
            MathUtils::roundUp(
                intersection->east() / projectedWidthPerPixel,
                kPixelTolerance) *
                projectedWidthPerPixel,
            latitudeAtProjectedY(scheme, roundedProjectedNorth));

        if (expanded.west() == expanded.east()) {
            expanded = Rectangle(
                expanded.west(),
                expanded.south(),
                expanded.east() + projectedWidthPerPixel,
                expanded.north());
        }
        if (projectedHeight(scheme, expanded) == 0.0) {
            expanded = Rectangle(
                expanded.west(),
                expanded.south(),
                expanded.east(),
                latitudeAtProjectedY(
                    scheme,
                    projectedNorth(scheme, expanded) +
                        projectedHeightPerPixel));
        }

        combinedBounds = combinedBounds
            ? combinedBounds->computeUnion(expanded)
            : expanded;
    }

    if (!combinedBounds) {
        return {};
    }

    int width = static_cast<int>(MathUtils::roundUp(
        combinedBounds->width() / projectedWidthPerPixel,
        kPixelTolerance));
    int height = static_cast<int>(MathUtils::roundUp(
        projectedHeight(scheme, *combinedBounds) / projectedHeightPerPixel,
        kPixelTolerance));
    width = std::clamp(width, 1, maximumTextureSize);
    height = std::clamp(height, 1, maximumTextureSize);
    return CombinedImageMeasurements{*combinedBounds, width, height};
}

void blitImage(DecodedImage& target,
               const Rectangle& targetRectangle,
               const DecodedImage& source,
               const Rectangle& sourceRectangle,
               const std::optional<Rectangle>& sourceSubset,
               const TileScheme& scheme) {
    const Rectangle sourceToCopy = sourceSubset.value_or(sourceRectangle);
    std::optional<Rectangle> overlap =
        targetRectangle.computeIntersection(sourceToCopy);
    if (!overlap) {
        return;
    }

    const PixelRectangle dst =
        computePixelRectangle(target, targetRectangle, *overlap, scheme);
    const PixelRectangle src =
        computePixelRectangle(source, sourceRectangle, *overlap, scheme);
    if (dst.width <= 0 || dst.height <= 0 ||
        src.width <= 0 || src.height <= 0) {
        return;
    }

    for (int y = 0; y < dst.height; ++y) {
        const int sy = std::clamp(
            src.y + static_cast<int>(
                        (static_cast<int64_t>(y) * src.height) / dst.height),
            0,
            source.height - 1);
        const int dy = dst.y + y;
        if (dy < 0 || dy >= target.height) {
            continue;
        }
        for (int x = 0; x < dst.width; ++x) {
            const int sx = std::clamp(
                src.x + static_cast<int>(
                            (static_cast<int64_t>(x) * src.width) / dst.width),
                0,
                source.width - 1);
            const int dx = dst.x + x;
            if (dx < 0 || dx >= target.width) {
                continue;
            }

            const size_t srcIndex =
                (static_cast<size_t>(sy) *
                     static_cast<size_t>(source.width) +
                 static_cast<size_t>(sx)) *
                static_cast<size_t>(source.channels);
            const size_t dstIndex =
                (static_cast<size_t>(dy) *
                     static_cast<size_t>(target.width) +
                 static_cast<size_t>(dx)) *
                4u;
            target.pixels[dstIndex + 0] = source.pixels[srcIndex + 0];
            target.pixels[dstIndex + 1] =
                source.channels > 1 ? source.pixels[srcIndex + 1]
                                    : source.pixels[srcIndex + 0];
            target.pixels[dstIndex + 2] =
                source.channels > 2 ? source.pixels[srcIndex + 2]
                                    : source.pixels[srcIndex + 0];
            target.pixels[dstIndex + 3] =
                source.channels >= 4 ? source.pixels[srcIndex + 3] : 255;
        }
    }
}

RasterOverlayTileProvider::CompositeImageResult combineQuadtreeSourceImages(
    const TileScheme& scheme,
    const Rectangle& targetBounds,
    int sourceZoom,
    std::vector<LoadedSourceImage>&& sources,
    int maximumSourceZoom,
    int maximumTextureSize) {
    (void)sourceZoom;
    sources.erase(
        std::remove_if(sources.begin(), sources.end(),
                       [](const LoadedSourceImage& source) {
                           return !source.image ||
                                  !isDecodedImageUploadable(*source.image) ||
                                  source.image->channels < 3;
                       }),
        sources.end());
    if (sources.empty()) return {};
    double projectedWidthPerPixel = std::numeric_limits<double>::max();
    double projectedHeightPerPixel = std::numeric_limits<double>::max();
    for (const LoadedSourceImage& source : sources) {
        projectedWidthPerPixel = std::min(
            projectedWidthPerPixel,
            source.bounds.width() / static_cast<double>(source.image->width));
        projectedHeightPerPixel = std::min(
            projectedHeightPerPixel,
            projectedHeight(scheme, source.bounds) /
                static_cast<double>(source.image->height));
    }
    if (projectedWidthPerPixel <= 0.0 || projectedHeightPerPixel <= 0.0 ||
        !std::isfinite(projectedWidthPerPixel) ||
        !std::isfinite(projectedHeightPerPixel)) {
        return {};
    }

    CombinedImageMeasurements measurements = measureCombinedImage(
        scheme,
        targetBounds,
        sources,
        projectedWidthPerPixel,
        projectedHeightPerPixel,
        maximumTextureSize);
    if (measurements.width <= 0 || measurements.height <= 0) {
        return {};
    }

    auto output = std::make_unique<DecodedImage>();
    output->width = measurements.width;
    output->height = measurements.height;
    output->channels = 4;
    output->pixels.resize(static_cast<size_t>(output->width) *
                          static_cast<size_t>(output->height) * 4u, 0);

    for (const LoadedSourceImage& source : sources) {
        blitImage(*output,
                  measurements.rectangle,
                  *source.image,
                  source.bounds,
                  source.sourceSubset,
                  scheme);
    }

    RasterOverlayTileProvider::CompositeImageResult result;
    result.image = std::move(output);
    result.rectangle = measurements.rectangle;
    const bool moreDetailAvailable = std::any_of(
        sources.begin(),
        sources.end(),
        [maximumSourceZoom](const LoadedSourceImage& source) {
            const RasterOverlayTile::MoreDetailAvailable sourceMoreDetail =
                source.moreDetailAvailable !=
                        RasterOverlayTile::MoreDetailAvailable::Unknown
                    ? source.moreDetailAvailable
                    : (source.key.z < maximumSourceZoom
                           ? RasterOverlayTile::MoreDetailAvailable::Yes
                           : RasterOverlayTile::MoreDetailAvailable::No);
            return !source.sourceSubset.has_value() &&
                   sourceMoreDetail == RasterOverlayTile::MoreDetailAvailable::Yes;
        });
    result.moreDetailAvailable =
        moreDetailAvailable
            ? RasterOverlayTile::MoreDetailAvailable::Yes
            : RasterOverlayTile::MoreDetailAvailable::No;
    return result;
}

using CompositeRequestSuccess =
    std::function<void(std::unique_ptr<DecodedImage>,
                       std::shared_ptr<const DecodedImage>,
                       Rectangle,
                       RasterOverlayTile::MoreDetailAvailable)>;
using CompositeRequestFailure = std::function<void()>;

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
        static_cast<size_t>(std::max(0, coverage.count())));
    for (const TileRange& coveredRange : coverage.ranges) {
        for (int y = coveredRange.minY; y <= coveredRange.maxY; ++y) {
            for (int x = coveredRange.minX; x <= coveredRange.maxX; ++x) {
                TileKey sourceKey{scheme.id(), plan.sourceZoom, x, y};
                if (provider.supportsTile(sourceKey) &&
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
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                TileKey sourceKey{scheme.id(), plan.sourceZoom, x, y};
                if (provider.supportsTile(sourceKey) &&
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
    using SourceReady = std::function<void(LoadedSourceImage&&)>;

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
        SourceReady onReady,
        std::vector<TileKey> fallbackInFlightKeys = {}) {
        std::optional<LoadedSourceImage> cachedSource;
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
                LoadedSourceImage source;
                source.key = it->second.key;
                source.bounds = it->second.bounds;
                source.image = it->second.image;
                source.sourceSubset = ancestorFallback
                    ? std::optional<Rectangle>(
                          scheme.tileToRectangle(originalKey))
                    : it->second.sourceSubset;
                source.moreDetailAvailable = it->second.moreDetailAvailable;
                cachedSource = std::move(source);
            }
        }
        if (cachedSource) {
            onReady(std::move(*cachedSource));
            return;
        }

        auto self = shared_from_this();
        if (shareInFlight) {
            const std::string inFlightKey = depotCacheKey(originalKey);
            auto waiter =
                [self, originalKey, ancestorFallback, onReady](
                    InFlightSourceTileAsset::Result cached) mutable {
                    onReady(self->loadedSourceFromAsset(
                        cached,
                        originalKey,
                        ancestorFallback));
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
                [self, originalKey, ancestorFallback, onReady](
                    InFlightSourceTileAsset::Result cached) mutable {
                    onReady(self->loadedSourceFromAsset(
                        cached,
                        originalKey,
                        ancestorFallback));
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

        onSourceIssued();
        CancellationToken token;
        provider.requestTile(
            requestedKey,
            token,
            [self,
             requestedKey,
             originalKey,
             ancestorFallback,
             onSourceIssued,
             onSourceFinished,
             onReady,
             fallbackInFlightKeys = std::move(fallbackInFlightKeys)](
                const TileKey& loadedKey,
                std::unique_ptr<DecodedImage> image) mutable {
                if (image) {
                    onSourceFinished();
                    LoadedSourceImage source;
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
                    self->cacheSource(originalKey, source);
                    auto completed = std::make_shared<SourceTileAsset>(
                        self->cachedSourceFromLoaded(source));
                    InFlightSourceTileAsset::Result directCompleted =
                        completed;
                    if (loadedKey != originalKey) {
                        LoadedSourceImage directSource;
                        directSource.key = loadedKey;
                        directSource.bounds = source.bounds;
                        directSource.image = source.image;
                        directSource.sourceSubset = std::nullopt;
                        directSource.moreDetailAvailable =
                            source.moreDetailAvailable;
                        self->cacheSource(loadedKey, directSource);
                        directCompleted = std::make_shared<SourceTileAsset>(
                            self->cachedSourceFromLoaded(directSource));
                    }
                    self->finishInFlightSource(originalKey, completed);
                    for (const TileKey& key : fallbackInFlightKeys) {
                        self->finishInFlightSource(key, directCompleted);
                    }
                    return;
                }

                if (requestedKey.z > self->minimumLevel) {
                    const TileKey parentKey = parentTileKey(requestedKey);
                    if (self->provider.supportsTile(parentKey)) {
                        onSourceFinished();
                        self->requestSource(
                            parentKey,
                            originalKey,
                            true,
                            false,
                            onSourceIssued,
                            onSourceFinished,
                            std::move(onReady),
                            std::move(fallbackInFlightKeys));
                        return;
                    }
                }

                onSourceFinished();
                auto failed = self->cacheTerminalFailure(originalKey);
                self->finishInFlightSource(originalKey, failed);
                for (const TileKey& key : fallbackInFlightKeys) {
                    self->finishInFlightSource(key, failed);
                }
            });
    }

private:
    LoadedSourceImage loadedSourceFromAsset(
        const InFlightSourceTileAsset::Result& cached,
        const TileKey& originalKey,
        bool ancestorFallback) const {
        if (!cached || !cached->image) {
            return LoadedSourceImage{};
        }
        LoadedSourceImage source;
        source.key = cached->key;
        source.bounds = cached->bounds;
        source.image = cached->image;
        source.sourceSubset = ancestorFallback
            ? std::optional<Rectangle>(scheme.tileToRectangle(originalKey))
            : cached->sourceSubset;
        source.moreDetailAvailable = cached->moreDetailAvailable;
        return source;
    }

    SourceTileAsset cachedSourceFromLoaded(
        const LoadedSourceImage& source) const {
        SourceTileAsset cached;
        cached.key = source.key;
        cached.bounds = source.bounds;
        if (source.image) {
            cached.image = source.image;
            cached.sizeBytes = decodedImageSizeBytes(*source.image);
        }
        cached.sourceSubset = source.sourceSubset;
        cached.moreDetailAvailable = source.moreDetailAvailable;
        return cached;
    }

    InFlightSourceTileAsset::Result cacheTerminalFailure(
        const TileKey& requestedKey) {
        SourceTileAsset failed;
        failed.key = requestedKey;
        failed.bounds = scheme.tileToRectangle(requestedKey);
        failed.moreDetailAvailable =
            RasterOverlayTile::MoreDetailAvailable::No;
        failed.terminalFailure = true;

        auto cached = std::make_shared<SourceTileAsset>(failed);
        std::lock_guard<std::mutex> lock(cacheMutex);
        const int64_t cacheBudgetBytes = state->subTileCacheBytes;
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
        cacheLru.emplace_back(key, failed.generation);
        cache[key] = failed;
        pruneCacheToBudget(cacheBudgetBytes);
        return cached;
    }

    void finishInFlightSource(const TileKey& originalKey,
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
    }

    void cacheSource(const TileKey& requestedKey,
                     const LoadedSourceImage& source) {
        if (!source.image) return;
        SourceTileAsset cached = cachedSourceFromLoaded(source);
        std::lock_guard<std::mutex> lock(cacheMutex);
        const int64_t cacheBudgetBytes = state->subTileCacheBytes;
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
        cacheLru.emplace_back(key, cached.generation);
        cache[key] = std::move(cached);
        pruneCacheToBudget(cacheBudgetBytes);
    }

    void touchCachedSource(const std::string& key, SourceTileAsset& source) {
        source.generation = ++cacheGeneration;
        cacheLru.emplace_back(key, source.generation);
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

struct RasterOverlayTileProvider::QuadtreeSourceRequest
    : public std::enable_shared_from_this<QuadtreeSourceRequest> {
    QuadtreeSourceRequest(const TileScheme& tileScheme,
                          std::shared_ptr<QuadtreeSourceAssetDepot> sourceDepot,
                          QuadtreeSourcePlan plan,
                          Rectangle bounds,
                          Rectangle outputRectangle,
                          int textureSize,
                          int maximumSourceLevel,
                          bool emptyWhenOnlyAncestorFallback,
                          CompositeRequestSuccess success,
                          CompositeRequestFailure failure)
        : scheme(tileScheme)
        , depot(std::move(sourceDepot))
        , sourcePlan(std::move(plan))
        , targetBounds(bounds)
        , outputBounds(outputRectangle)
        , maximumTextureSize(textureSize)
        , maximumLevel(maximumSourceLevel)
        , returnEmptyForAncestorOnly(emptyWhenOnlyAncestorFallback)
        , onSuccess(std::move(success))
        , onFailure(std::move(failure))
        , remaining(sourcePlan.budgetUnits()) {
        sources.reserve(sourcePlan.sourceKeys.size());
    }

    void issueAll(const std::function<void()>& onSourceIssued,
                  const std::function<void()>& onSourceFinished) {
        while (!isComplete()) {
            TileKey sourceKey;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (nextSourceIndex >= sourcePlan.sourceKeys.size()) {
                    break;
                }
                sourceKey = sourcePlan.sourceKeys[nextSourceIndex++];
            }
            auto self = shared_from_this();
            depot->requestSource(
                sourceKey,
                sourceKey,
                false,
                true,
                onSourceIssued,
                onSourceFinished,
                [self](LoadedSourceImage&& source) {
                    self->finishOneSource(std::move(source));
                });
        }
    }

    bool isComplete() const {
        std::lock_guard<std::mutex> lock(mutex);
        return completed;
    }

private:
    void finishOneSource(LoadedSourceImage&& source) {
        bool finished = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (source.image) {
                sources.push_back(std::move(source));
            }
            --remaining;
            finished = remaining == 0;
            completed = finished;
        }

        if (!finished) return;

        std::vector<LoadedSourceImage> completedSources;
        {
            std::lock_guard<std::mutex> lock(mutex);
            completedSources = std::move(sources);
        }

        if (completedSources.size() == 1 &&
            sourcePlan.sourceKeys.size() == 1 &&
            completedSources.front().image &&
            !completedSources.front().sourceSubset.has_value() &&
            rectanglesEqualForDirectRasterTile(
                targetBounds,
                completedSources.front().bounds)) {
            LoadedSourceImage& source = completedSources.front();
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
                outputBounds,
                moreDetailAvailable);
            return;
        }

        if (completedSources.empty()) {
            onFailure();
            return;
        }

        const bool haveAnyUsefulImageData =
            !returnEmptyForAncestorOnly ||
            std::any_of(
                completedSources.begin(),
                completedSources.end(),
                [](const LoadedSourceImage& source) {
                    return source.image && !source.sourceSubset.has_value();
                });
        if (!haveAnyUsefulImageData) {
            onSuccess(
                std::make_unique<DecodedImage>(),
                nullptr,
                Rectangle(),
                RasterOverlayTile::MoreDetailAvailable::No);
            return;
        }

        CompositeImageResult composed =
            combineQuadtreeSourceImages(
                scheme,
                targetBounds,
                sourcePlan.sourceZoom,
                std::move(completedSources),
                maximumLevel,
                maximumTextureSize);
        if (composed.image) {
            onSuccess(
                std::move(composed.image),
                nullptr,
                outputBounds,
                composed.moreDetailAvailable);
        } else {
            onFailure();
        }
    }

    const TileScheme& scheme;
    std::shared_ptr<QuadtreeSourceAssetDepot> depot;
    QuadtreeSourcePlan sourcePlan;
    Rectangle targetBounds;
    Rectangle outputBounds;
    int maximumTextureSize = 0;
    int maximumLevel = 0;
    bool returnEmptyForAncestorOnly = false;
    CompositeRequestSuccess onSuccess;
    CompositeRequestFailure onFailure;
    mutable std::mutex mutex;
    size_t nextSourceIndex = 0;
    int remaining = 0;
    bool completed = false;
    std::vector<LoadedSourceImage> sources;
};

RasterOverlayTileProvider::CompositeImageResult
RasterOverlayTileProvider::composeQuadtreeSourceImagesWithDetails(
    const TileScheme& scheme,
    const Rectangle& targetBounds,
    int sourceZoom,
    std::vector<QuadtreeSourceImage>&& publicSources,
    int maximumSourceZoom,
    int maximumTextureSize) {
    std::vector<LoadedSourceImage> sources;
    sources.reserve(publicSources.size());
    for (auto& source : publicSources) {
        sources.push_back(LoadedSourceImage{
            source.key,
            source.bounds,
            std::shared_ptr<const DecodedImage>(std::move(source.image)),
            source.sourceSubset,
            source.moreDetailAvailable});
    }
    return combineQuadtreeSourceImages(
        scheme,
        targetBounds,
        sourceZoom,
        std::move(sources),
        maximumSourceZoom,
        maximumTextureSize);
}

double RasterOverlayTileProvider::projectedVForLatitude(
    const TileScheme& scheme,
    const Rectangle& bounds,
    double lat) {
    return projectedVForLatitudeInternal(scheme, bounds, lat);
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
}

void RasterOverlayTileProvider::setOwner(RasterOverlay* owner) {
    owner_ = owner;
    if (owner_) {
        setCoverageRectangle(owner_->getOptions().coverageRectangle);
        setMaximumScreenSpaceError(
            owner_->getOptions().maximumScreenSpaceError);
        setMaximumTextureSize(owner_->getOptions().maximumTextureSize);
        setSubTileCacheBytes(owner_->getOptions().subTileCacheBytes);
        setLevelRange(owner_->getOptions().minimumZoom,
                      owner_->getOptions().maximumZoom);
    }
}

void RasterOverlayTileProvider::setCoverageRectangle(
    const Rectangle& coverageRectangle) {
    if (coverageRectangle_ == coverageRectangle) {
        return;
    }
    coverageRectangle_ = coverageRectangle;
    invalidateCompositeTileCache();
}

void RasterOverlayTileProvider::setMaximumScreenSpaceError(
    double maximumScreenSpaceError) {
    const double nextMaximumScreenSpaceError =
        maximumScreenSpaceError > 0.0 ? maximumScreenSpaceError : 2.0;
    if (maximumScreenSpaceError_ == nextMaximumScreenSpaceError) {
        return;
    }
    maximumScreenSpaceError_ = nextMaximumScreenSpaceError;
    invalidateCompositeTileCache();
}

void RasterOverlayTileProvider::setMaximumTextureSize(int maximumTextureSize) {
    const int nextMaximumTextureSize =
        maximumTextureSize > 0 ? maximumTextureSize : 2048;
    if (maximumTextureSize_ == nextMaximumTextureSize) {
        return;
    }
    maximumTextureSize_ = nextMaximumTextureSize;
    invalidateCompositeTileCache();
}

void RasterOverlayTileProvider::setSubTileCacheBytes(int64_t subTileCacheBytes) {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    asyncState_->subTileCacheBytes = std::max<int64_t>(0, subTileCacheBytes);
    while (asyncState_->sourceTileDepotCacheBytes >
               asyncState_->subTileCacheBytes &&
           !asyncState_->sourceTileDepotCacheLru.empty()) {
        auto [key, generation] =
            asyncState_->sourceTileDepotCacheLru.front();
        asyncState_->sourceTileDepotCacheLru.pop_front();
        auto it = asyncState_->sourceTileDepotCache.find(key);
        if (it == asyncState_->sourceTileDepotCache.end() ||
            it->second.generation != generation) {
            continue;
        }
        asyncState_->sourceTileDepotCacheBytes -= it->second.sizeBytes;
        asyncState_->sourceTileDepotCache.erase(it);
    }
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
    invalidateCompositeTileCache();
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

void RasterOverlayTileProvider::invalidateCompositeTileCache() {
    ++compositeTileEpoch_;
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        if (it->first.rfind("composite/", 0) == 0 &&
            it->second &&
            it->second->getState() !=
                RasterOverlayTile::LoadState::Loading) {
            it = tiles_.erase(it);
        } else {
            ++it;
        }
    }
    asyncState_->revision.fetch_add(1, std::memory_order_relaxed);
}

void RasterOverlayTileProvider::invalidateSourceAssetDepotCache() {
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        ++asyncState_->sourceTileDepotEpoch;
        asyncState_->sourceTileDepotCache.clear();
        asyncState_->sourceTileDepotCacheLru.clear();
        asyncState_->sourceTileDepotCacheBytes = 0;
    }
    refreshSourceAssetDepot();
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
    return key.schemeId + "/" + std::to_string(key.z) + "/" +
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
    if (!geographicBounds.computeIntersection(coverageRectangle_)) return nullptr;
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
        return {getPlaceholderTile(), false};
    }

    const Rectangle geometryBounds =
        unprojectProviderToGeographic(providerGeometryBounds, projection_);
    const std::optional<Rectangle> sourceBounds =
        mapGeometryBoundsToImageryCoverage(
            geometryBounds,
            coverageRectangle_,
            shouldClampOutsideCoverage(owner_));
    if (!sourceBounds) {
        return {nullptr, false};
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
        return {getPlaceholderTile(), false};
    }

    if (sourcePlan.sourceKeys.size() == 1) {
        const TileKey& sourceKey = sourcePlan.sourceKeys.front();
        const Rectangle sourceTileBounds = scheme_.tileToRectangle(sourceKey);
        if (rectanglesEqualForDirectRasterTile(geometryBounds,
                                               sourceTileBounds) &&
            rectanglesEqualForDirectRasterTile(*sourceBounds,
                                               sourceTileBounds)) {
            return {getTile(sourceKey), true};
        }
    }

    const std::string ck = compositeTileCacheKey(
        scheme_,
        providerGeometryBounds,
        sourcePlan.sourceZoom,
        compositeTileEpoch_);
    auto existing = tiles_.find(ck);
    if (existing != tiles_.end()) {
        existing->second->lastUsedFrame = frameNumber_;
        return {existing->second, false};
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
    tile->setCompositeTileSourceLevel(sourcePlan.sourceZoom);
    tile->setTargetScreenPixels(targetScreenPixelsX, targetScreenPixelsY);
    tile->lastUsedFrame = frameNumber_;
    tiles_[ck] = tile;
    return {tile, false};
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

        // Check if tile is loaded (has a texture)
        if (tile && tile->getState() >= RasterOverlayTile::LoadState::Loaded) {
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
    int count = 0;
    for (const auto& [key, tile] : tiles_) {
        if (tile->getState() == RasterOverlayTile::LoadState::Loading) {
            ++count;
        }
    }
    return count;
}

int RasterOverlayTileProvider::getPendingUploadCount() const {
    std::lock_guard<std::mutex> lock(asyncState_->mutex);
    return static_cast<int>(asyncState_->pendingUploads.size());
}

bool RasterOverlayTileProvider::loadTile(RasterOverlayTile& tile,
                                         FrameResourceBudget* budget) {
    if (tile.isCompositeTile()) {
        return loadCompositeTile(tile, budget);
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
    QuadtreeSourcePlan sourcePlan;
    sourcePlan.sourceZoom = key.z;
    sourcePlan.minX = key.x;
    sourcePlan.minY = key.y;
    sourcePlan.maxX = key.x;
    sourcePlan.maxY = key.y;
    sourcePlan.sourceKeys.push_back(key);
    const Rectangle outputBounds = tile.getRectangle();
    const Rectangle targetBounds =
        unprojectProviderToGeographic(outputBounds, projection_);
    return loadMappedTile(
        tile,
        std::move(sourcePlan),
        targetBounds,
        outputBounds,
        tileCacheKey(key),
        budget);
}

bool RasterOverlayTileProvider::loadTileThrottled(RasterOverlayTile& tile,
                                                  FrameResourceBudget* budget) {
    // cesium-native: loadTileThrottled only starts Unloaded tiles. Once a
    // composite tile is Loading, its source dependencies are already attached
    // to provider-level source tile assets and do not need per-frame pumping.
    if (tile.getState() != RasterOverlayTile::LoadState::Unloaded) {
        return true;
    }

    if (getThrottledTilesCurrentlyLoading() >= maximumSimultaneousTileLoads) {
        return false;  // Throttled
    }

    return loadTile(tile, budget);
}

bool RasterOverlayTileProvider::loadCompositeTile(RasterOverlayTile& tile,
                                                  FrameResourceBudget* budget) {
    auto loadState = tile.getState();
    switch (loadState) {
        case RasterOverlayTile::LoadState::Unloaded:
            break;
        case RasterOverlayTile::LoadState::Loading:
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
    const std::optional<Rectangle> sourceBounds =
        mapGeometryBoundsToImageryCoverage(
            targetBounds,
            coverageRectangle_,
            shouldClampOutsideCoverage(owner_));
    if (!sourceBounds) {
        logAndroidRasterPipeline("coverage-miss", ck, 0, 0);
        tile.setMoreDetailAvailable(RasterOverlayTile::MoreDetailAvailable::No);
        tile.setState(RasterOverlayTile::LoadState::Failed);
        return false;
    }

    QuadtreeSourcePlan sourcePlan = buildQuadtreeSourcePlan(
        scheme_,
        provider_,
        textureUploader_.get(),
        targetBounds,
        *sourceBounds,
        tile.getTargetScreenPixelsX(),
        tile.getTargetScreenPixelsY(),
        maximumScreenSpaceError_,
        maximumTextureSize_,
        getMinimumLevel(),
        getMaximumLevel());

    if (sourcePlan.empty()) {
        logAndroidRasterPipeline("empty-plan", ck, 0, sourcePlan.sourceZoom);
        tile.setMoreDetailAvailable(RasterOverlayTile::MoreDetailAvailable::No);
        tile.setState(RasterOverlayTile::LoadState::Failed);
        return false;
    }
    logAndroidRasterPipeline(
        "start",
        ck,
        static_cast<int>(sourcePlan.sourceKeys.size()),
        sourcePlan.sourceZoom);
    return loadMappedTile(
        tile,
        std::move(sourcePlan),
        *sourceBounds,
        outputBounds,
        ck,
        budget);
}

bool RasterOverlayTileProvider::loadMappedTile(
    RasterOverlayTile& tile,
    QuadtreeSourcePlan sourcePlan,
    const Rectangle& targetBounds,
    const Rectangle& outputBounds,
    const std::string& cacheKey,
    FrameResourceBudget* budget) {
    if (cacheKey.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        if (asyncState_->inFlightRequests.count(cacheKey)) return true;
    }

    auto estimateNewSourceRequests = [&]() {
        int estimated = 0;
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        for (const TileKey& sourceKey : sourcePlan.sourceKeys) {
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
            if (asyncState_->sourceTileDepotInFlight.count(sourceKeyString) >
                0) {
                continue;
            }
            ++estimated;
        }
        return estimated;
    };
    if (!tryIssueRasterRequestBudget(
            budget,
            asyncState_->activeRasterSourceRequests,
            estimateNewSourceRequests())) {
        return false;
    }

    tile.setState(RasterOverlayTile::LoadState::Loading);
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
    const int maxTextureSize =
        maximumCombinedTextureSize(textureUploader_.get(), maximumTextureSize_);
    if (!sourceAssetDepot_) {
        refreshSourceAssetDepot();
    }
    const bool returnEmptyForAncestorOnly = true;
    auto request = std::make_shared<QuadtreeSourceRequest>(
        scheme_,
        sourceAssetDepot_,
        sourcePlan,
        targetBounds,
        outputBounds,
        maxTextureSize,
        getMaximumLevel(),
        returnEmptyForAncestorOnly,
        [state, cacheKey](std::unique_ptr<DecodedImage> composed,
                          std::shared_ptr<const DecodedImage> sharedImage,
                          Rectangle rectangle,
                          RasterOverlayTile::MoreDetailAvailable moreDetailAvailable) {
            std::lock_guard<std::mutex> providerLock(state->mutex);
            state->inFlightRequests.erase(cacheKey);
            if (!state->alive.load(std::memory_order_acquire)) {
                return;
            }
            logAndroidRasterPipeline("composed", cacheKey, 0, 0);
            state->pendingUploads.push_back(
                {cacheKey,
                 std::move(composed),
                 std::move(sharedImage),
                 rectangle,
                 moreDetailAvailable});
        },
        [state, cacheKey, tileWeak]() {
            std::lock_guard<std::mutex> providerLock(state->mutex);
            state->inFlightRequests.erase(cacheKey);
            logAndroidRasterPipeline("compose-failed", cacheKey, 0, 0);
            if (auto tile = tileWeak.lock()) {
                tile->setMoreDetailAvailable(
                    RasterOverlayTile::MoreDetailAvailable::No);
                tile->setState(RasterOverlayTile::LoadState::Failed);
            }
            auto& fr = state->failedTiles[cacheKey];
            if (fr.firstFailTime == 0.0) {
                fr.firstFailTime = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            }
            fr.retries++;
            state->revision.fetch_add(1, std::memory_order_relaxed);
        });

    request->issueAll(
        [state]() {
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
        },
        [state]() {
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
        });

    return true;
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

    int processed = 0;
    while (true) {
        PendingUpload upload;
        {
            std::lock_guard<std::mutex> lock(asyncState_->mutex);
            // Composite raster tiles can be 512x512+ and state finalization
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
            asyncState_->pendingUploads.erase(selected);
        }

        std::vector<TilePtr> targetTiles;
        if (auto it = tiles_.find(upload.cacheKey); it != tiles_.end()) {
            targetTiles.push_back(it->second);
        }
        if (targetTiles.empty()) continue;

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
            // Resource-prep upload (main-thread safe). Composite images are
            // already combined at the selector's target screen-pixel density;
            // on mobile, generating mipmaps for every composite image is
            // expensive main-thread work without improving the current
            // selected tile.
            const bool generateMipmaps = !tile.isCompositeTile();
            RasterTextureUploadOptions uploadOptions;
            uploadOptions.generateMipmaps = generateMipmaps;
            auto tex = textureUploader_
                ? textureUploader_->uploadRasterTexture(
                      *uploadImage,
                      uploadOptions)
                : nullptr;
            uploadMs = perf::nowMs() - uploadStartMs;
            if (!tex) {
                tile.setMoreDetailAvailable(
                    RasterOverlayTile::MoreDetailAvailable::No);
                tile.setState(RasterOverlayTile::LoadState::Failed);
                continue;
            }
#ifndef __ANDROID__
            (void)uploadMs;
#endif
            const int sourceLevel =
                tile.isCompositeTile() ? tile.getSourceZoom() : tile.getTileID().z;
            const RasterOverlayTile::MoreDetailAvailable moreDetailAvailable =
                upload.moreDetailAvailable !=
                        RasterOverlayTile::MoreDetailAvailable::Unknown
                    ? upload.moreDetailAvailable
                    : (sourceLevel < tile.getMaxZoom()
                           ? RasterOverlayTile::MoreDetailAvailable::Yes
                           : RasterOverlayTile::MoreDetailAvailable::No);
            tile.setMoreDetailAvailable(moreDetailAvailable);
            tile.setRectangle(upload.rectangle);
            // cesium-native: transfer texture ownership to the tile.
            // The tile owns its texture; no external cache needed.
            tile.setTexture(std::move(tex));
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_INFO, "RasterOverlayTileProvider",
                "Tile loaded: %d/%d/%d", tile.getTileID().z,
                tile.getTileID().x, tile.getTileID().y);
            if (uploadMs >= 8.0 ||
                uploadImage->width > 1024 ||
                uploadImage->height > 1024) {
                __android_log_print(ANDROID_LOG_INFO, "RasterOverlayTileProvider",
                    "upload %.2fms size=%dx%d composite=%d mipmap=%d cache=%s",
                    uploadMs,
                    uploadImage->width,
                    uploadImage->height,
                    tile.isCompositeTile() ? 1 : 0,
                    generateMipmaps ? 1 : 0,
                    tile.getCacheKey().c_str());
            }
#endif
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
           !asyncState_->inFlightRequests.empty();
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

void RasterOverlayTileProvider::trimUnusedTiles() {
    // Keep recently referenced tiles for a short window. cesium-native retains
    // raster tiles via intrusive references and a cache budget; this local
    // provider owns tiles directly, so immediate one-frame eviction would
    // churn active mapping handles and waste in-flight IO.
    for (auto it = tiles_.begin(); it != tiles_.end(); ) {
        RasterOverlayTile& tile = *it->second;
        const uint64_t age = frameNumber_ > tile.lastUsedFrame
            ? frameNumber_ - tile.lastUsedFrame
            : 0;
        bool inFlight = false;
        {
            std::lock_guard<std::mutex> lock(asyncState_->mutex);
            inFlight = asyncState_->inFlightRequests.count(it->first) > 0;
        }
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
