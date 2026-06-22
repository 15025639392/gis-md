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
    if (cacheKey.rfind("rectangle/", 0) == 0) {
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

struct RectangleSourcePlan {
    int sourceZoom = 0;
    TileRange range;
    std::vector<TileKey> sourceKeys;

    int budgetUnits() const {
        return static_cast<int>(sourceKeys.size());
    }

    bool empty() const { return sourceKeys.empty(); }
};

bool tryIssueRasterRequestBudget(FrameResourceBudget* budget,
                                 uint32_t currentInflight,
                                 int estimatedFanout) {
    if (estimatedFanout <= 0) {
        return true;
    }
    if (!budget) {
        return true;
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

Rectangle mapGeometryBoundsToImageryCoverage(const Rectangle& geometryBounds,
                                             const Rectangle& coverage) {
    std::optional<Rectangle> intersection =
        geometryBounds.computeIntersection(coverage);
    if (intersection) {
        return *intersection;
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

std::unique_ptr<DecodedImage> cloneDecodedImage(const DecodedImage& image) {
    auto clone = std::make_unique<DecodedImage>();
    clone->width = image.width;
    clone->height = image.height;
    clone->channels = image.channels;
    clone->pixels = image.pixels;
    return clone;
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

int chooseRectangleSourceZoom(const TileScheme& scheme,
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

    TileRange range = trimCesiumNativeBoundarySlop(
        scheme,
        geometryBounds,
        zoom,
        computeRange(scheme, sourceBounds, zoom));
    while (zoom > minZoom) {
        const int widthPixels = range.width() * std::max(1, provider.tileWidth());
        const int heightPixels = range.height() * std::max(1, provider.tileHeight());
        if (widthPixels <= maxTextureSize &&
            heightPixels <= maxTextureSize) {
            break;
        }
        --zoom;
        range = trimCesiumNativeBoundarySlop(
            scheme,
            geometryBounds,
            zoom,
            computeRange(scheme, sourceBounds, zoom));
    }

    if (outRange) *outRange = range;
    return zoom;
}

RectangleSourcePlan buildRectangleSourcePlan(
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
    RectangleSourcePlan plan;
    plan.sourceZoom = chooseRectangleSourceZoom(
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
        &plan.range);
    plan.sourceKeys.reserve(
        static_cast<size_t>(std::max(0, plan.range.count())));
    for (int y = plan.range.minY; y <= plan.range.maxY; ++y) {
        for (int x = plan.range.minX; x <= plan.range.maxX; ++x) {
            TileKey sourceKey{scheme.id(), plan.sourceZoom, x, y};
            if (provider.supportsTile(sourceKey) &&
                rectanglesOverlapWithArea(
                    scheme.tileToRectangle(sourceKey),
                    sourceBounds)) {
                plan.sourceKeys.push_back(sourceKey);
            }
        }
    }
    if (plan.sourceKeys.empty() &&
        sourceBounds.width() > 0.0 &&
        sourceBounds.height() > 0.0) {
        const auto center = sourceBounds.center();
        TileKey sourceKey =
            scheme.positionToTile(center.first, center.second, plan.sourceZoom);
        if (provider.supportsTile(sourceKey)) {
            plan.range = TileRange{
                sourceKey.x,
                sourceKey.y,
                sourceKey.x,
                sourceKey.y};
            plan.sourceKeys.push_back(sourceKey);
        }
    }
    return plan;
}

std::string rectangleTileCacheKey(const TileScheme& scheme,
                                  const Rectangle& rectangle,
                                  int sourceZoom) {
    char bounds[256];
    std::snprintf(bounds,
                  sizeof(bounds),
                  "%.17g/%.17g/%.17g/%.17g",
                  rectangle.west(),
                  rectangle.south(),
                  rectangle.east(),
                  rectangle.north());
    return "rectangle/" + scheme.id() + "/srcz/" +
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

RasterOverlayTileProvider::RectangleCompositionResult combineRectangleImages(
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
    const bool haveAnyUsefulImageData =
        std::any_of(sources.begin(),
                    sources.end(),
                    [](const LoadedSourceImage& source) {
                        return source.image &&
                               !source.sourceSubset.has_value();
                    });
    if (!haveAnyUsefulImageData) {
        RasterOverlayTileProvider::RectangleCompositionResult result;
        result.image = std::make_unique<DecodedImage>();
        result.rectangle = Rectangle();
        result.moreDetailAvailable =
            RasterOverlayTile::MoreDetailAvailable::No;
        return result;
    }

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

    RasterOverlayTileProvider::RectangleCompositionResult result;
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

using RectangleRequestSuccess =
    std::function<void(std::unique_ptr<DecodedImage>,
                       Rectangle,
                       RasterOverlayTile::MoreDetailAvailable)>;
using RectangleRequestFailure = std::function<void()>;

} // namespace

struct RasterOverlayTileProvider::RectangleSourceRequest
    : public std::enable_shared_from_this<RectangleSourceRequest> {
    RectangleSourceRequest(ImageryProvider& imageryProvider,
                           const TileScheme& tileScheme,
                           std::shared_ptr<ProviderAsyncState> asyncState,
                           RectangleSourcePlan plan,
                           Rectangle bounds,
                           Rectangle outputRectangle,
                           int textureSize,
                           int minimumSourceLevel,
                           int maximumSourceLevel,
                           RectangleRequestSuccess success,
                           RectangleRequestFailure failure)
        : provider(imageryProvider)
        , scheme(tileScheme)
        , state(std::move(asyncState))
        , cache(state->sourceTileDepotCache)
        , cacheLru(state->sourceTileDepotCacheLru)
        , cacheBytes(state->sourceTileDepotCacheBytes)
        , cacheGeneration(state->sourceTileDepotGeneration)
        , inFlight(state->sourceTileDepotInFlight)
        , cacheBudgetBytes(state->subTileCacheBytes)
        , cacheMutex(state->mutex)
        , sourcePlan(std::move(plan))
        , targetBounds(bounds)
        , outputBounds(outputRectangle)
        , maximumTextureSize(textureSize)
        , minimumLevel(minimumSourceLevel)
        , maximumLevel(maximumSourceLevel)
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
            requestSource(sourceKey,
                          sourceKey,
                          false,
                          true,
                          onSourceIssued,
                          onSourceFinished);
        }
    }

    bool isComplete() const {
        std::lock_guard<std::mutex> lock(mutex);
        return completed;
    }

private:
    void requestSource(
        const TileKey& requestedKey,
        const TileKey& originalKey,
        bool ancestorFallback,
        bool shareInFlight,
        const std::function<void()>& onSourceIssued,
        const std::function<void()>& onSourceFinished,
        std::vector<TileKey> fallbackInFlightKeys = {}) {
        std::optional<LoadedSourceImage> cachedSource;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            const std::string originalCacheKey = sourceCacheKey(originalKey);
            auto it = cache.find(originalCacheKey);
            if (it == cache.end() && ancestorFallback) {
                it = cache.find(sourceCacheKey(requestedKey));
            }
            if (it != cache.end() && it->second.image) {
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
            finishOneSource(std::move(*cachedSource));
            return;
        }

        auto self = shared_from_this();
        if (shareInFlight) {
            const std::string inFlightKey = sourceCacheKey(originalKey);
            auto waiter =
                [self, originalKey, ancestorFallback](
                    InFlightSourceTileAsset::Result cached) {
                    if (cached && cached->image) {
                        LoadedSourceImage source;
                        source.key = cached->key;
                        source.bounds = cached->bounds;
                        source.image = cached->image;
                        source.sourceSubset = ancestorFallback
                            ? std::optional<Rectangle>(
                                  self->scheme.tileToRectangle(originalKey))
                            : cached->sourceSubset;
                        source.moreDetailAvailable =
                            cached->moreDetailAvailable;
                        self->finishOneSource(std::move(source));
                    } else {
                        self->finishOneSource(LoadedSourceImage{});
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
            const std::string fallbackInFlightKey = sourceCacheKey(requestedKey);
            auto waiter =
                [self, originalKey, ancestorFallback](
                    InFlightSourceTileAsset::Result cached) {
                    if (cached && cached->image) {
                        LoadedSourceImage source;
                        source.key = cached->key;
                        source.bounds = cached->bounds;
                        source.image = cached->image;
                        source.sourceSubset = ancestorFallback
                            ? std::optional<Rectangle>(
                                  self->scheme.tileToRectangle(originalKey))
                            : cached->sourceSubset;
                        source.moreDetailAvailable =
                            cached->moreDetailAvailable;
                        self->finishOneSource(std::move(source));
                    } else {
                        self->finishOneSource(LoadedSourceImage{});
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

                // cesium-native QuadtreeRasterOverlayTileProvider:
                // failed sub-tiles try their parent before reporting an empty
                // contribution to the combined geometry image.
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
                            std::move(fallbackInFlightKeys));
                        return;
                    }
                }

                onSourceFinished();
                self->finishInFlightSource(originalKey, nullptr);
                for (const TileKey& key : fallbackInFlightKeys) {
                    self->finishInFlightSource(key, nullptr);
                }
            });
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

    void finishInFlightSource(const TileKey& originalKey,
                              InFlightSourceTileAsset::Result source) {
        std::vector<std::function<void(InFlightSourceTileAsset::Result)>>
            waiters;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            auto it = inFlight.find(sourceCacheKey(originalKey));
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
        if (cacheBudgetBytes <= 0) {
            std::lock_guard<std::mutex> lock(cacheMutex);
            cache.clear();
            cacheLru.clear();
            cacheBytes = 0;
            return;
        }
        SourceTileAsset cached;
        cached.key = source.key;
        cached.bounds = source.bounds;
        cached.image = source.image;
        cached.sourceSubset = source.sourceSubset;
        cached.moreDetailAvailable = source.moreDetailAvailable;
        cached.sizeBytes = decodedImageSizeBytes(*source.image);
        std::lock_guard<std::mutex> lock(cacheMutex);
        const std::string key = sourceCacheKey(requestedKey);
        auto existing = cache.find(key);
        if (existing != cache.end()) {
            cacheBytes -= existing->second.sizeBytes;
        }
        cached.generation = ++cacheGeneration;
        cacheBytes += cached.sizeBytes;
        cacheLru.emplace_back(key, cached.generation);
        cache[key] = std::move(cached);
        pruneCacheToBudget();
    }

    void touchCachedSource(const std::string& key, SourceTileAsset& source) {
        source.generation = ++cacheGeneration;
        cacheLru.emplace_back(key, source.generation);
    }

    void pruneCacheToBudget() {
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
                cloneDecodedImage(*source.image),
                outputBounds,
                moreDetailAvailable);
            return;
        }

        RectangleCompositionResult composed =
            combineRectangleImages(
                scheme,
                targetBounds,
                sourcePlan.sourceZoom,
                std::move(completedSources),
                maximumLevel,
                maximumTextureSize);
        if (composed.image) {
            onSuccess(
                std::move(composed.image),
                outputBounds,
                composed.moreDetailAvailable);
        } else {
            onFailure();
        }
    }

    ImageryProvider& provider;
    const TileScheme& scheme;
    std::shared_ptr<ProviderAsyncState> state;
    std::unordered_map<std::string, SourceTileAsset>& cache;
    std::deque<std::pair<std::string, uint64_t>>& cacheLru;
    int64_t& cacheBytes;
    uint64_t& cacheGeneration;
    std::unordered_map<std::string, InFlightSourceTileAsset>& inFlight;
    int64_t cacheBudgetBytes = 0;
    std::mutex& cacheMutex;
    RectangleSourcePlan sourcePlan;
    Rectangle targetBounds;
    Rectangle outputBounds;
    int maximumTextureSize = 0;
    int minimumLevel = 0;
    int maximumLevel = 0;
    RectangleRequestSuccess onSuccess;
    RectangleRequestFailure onFailure;
    mutable std::mutex mutex;
    size_t nextSourceIndex = 0;
    int remaining = 0;
    bool completed = false;
    std::vector<LoadedSourceImage> sources;
};

std::unique_ptr<DecodedImage>
RasterOverlayTileProvider::composeRectangleImages(
    const TileScheme& scheme,
    const Rectangle& targetBounds,
    int sourceZoom,
    std::vector<RectangleSourceImage>&& publicSources,
    int maximumTextureSize) {
    return composeRectangleImagesWithDetails(
        scheme,
        targetBounds,
        sourceZoom,
        std::move(publicSources),
        sourceZoom,
        maximumTextureSize).image;
}

RasterOverlayTileProvider::RectangleCompositionResult
RasterOverlayTileProvider::composeRectangleImagesWithDetails(
    const TileScheme& scheme,
    const Rectangle& targetBounds,
    int sourceZoom,
    std::vector<RectangleSourceImage>&& publicSources,
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
    return combineRectangleImages(
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
    , textureUploader_(std::move(textureUploader)) {}

RasterOverlayTileProvider::~RasterOverlayTileProvider() {
    asyncState_->alive.store(false, std::memory_order_release);
}

void RasterOverlayTileProvider::setOwner(RasterOverlay* owner) {
    owner_ = owner;
    if (owner_) {
        coverageRectangle_ = owner_->getOptions().coverageRectangle;
        setMaximumTextureSize(owner_->getOptions().maximumTextureSize);
        setSubTileCacheBytes(owner_->getOptions().subTileCacheBytes);
        setLevelRange(owner_->getOptions().minimumZoom,
                      owner_->getOptions().maximumZoom);
    }
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

int RasterOverlayTileProvider::getMinimumLevel() const {
    return std::max({scheme_.minZoom(), provider_.minZoom(), minimumLevel_});
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

RasterOverlayTileProvider::TilePtr RasterOverlayTileProvider::getTile(
    const Rectangle& providerGeometryBounds,
    double targetScreenPixelsX,
    double targetScreenPixelsY) {
    // cesium-native: return placeholder if provider is not yet ready
    if (!ready_) {
        return getPlaceholderTile();
    }

    const Rectangle geometryBounds =
        unprojectProviderToGeographic(providerGeometryBounds, projection_);
    const Rectangle sourceBounds =
        mapGeometryBoundsToImageryCoverage(geometryBounds, coverageRectangle_);

    RectangleSourcePlan sourcePlan = buildRectangleSourcePlan(
        scheme_,
        provider_,
        textureUploader_.get(),
        geometryBounds,
        sourceBounds,
        targetScreenPixelsX,
        targetScreenPixelsY,
        maximumScreenSpaceError_,
        maximumTextureSize_,
        getMinimumLevel(),
        getMaximumLevel());

    if (sourcePlan.sourceKeys.size() == 1) {
        const TileKey& sourceKey = sourcePlan.sourceKeys.front();
        const Rectangle sourceTileBounds = scheme_.tileToRectangle(sourceKey);
        if (rectanglesEqualForDirectRasterTile(geometryBounds,
                                               sourceTileBounds) &&
            rectanglesEqualForDirectRasterTile(sourceBounds,
                                               sourceTileBounds)) {
            return getTile(sourceKey);
        }
    }

    const std::string ck = rectangleTileCacheKey(
        scheme_, providerGeometryBounds, sourcePlan.sourceZoom);
    auto existing = tiles_.find(ck);
    if (existing != tiles_.end()) {
        existing->second->lastUsedFrame = frameNumber_;
        return existing->second;
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
    tile->setRectangleTileLevel(sourcePlan.sourceZoom);
    tile->setTargetScreenPixels(targetScreenPixelsX, targetScreenPixelsY);
    tile->lastUsedFrame = frameNumber_;
    tiles_[ck] = tile;
    return tile;
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
    return provider_.requestDiagnostics();
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
    if (tile.isRectangleTile()) {
        return loadRectangleTile(tile, budget);
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
    std::string ck = tileCacheKey(key);

    // Check if already in-flight
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        if (asyncState_->inFlightRequests.count(ck)) {
            return true;
        }
    }
    RectangleSourcePlan sourcePlan;
    sourcePlan.sourceZoom = key.z;
    sourcePlan.range = TileRange{key.x, key.y, key.x, key.y};
    sourcePlan.sourceKeys.push_back(key);
    auto estimateNewSourceRequests = [&]() {
        int estimated = 0;
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        for (const TileKey& sourceKey : sourcePlan.sourceKeys) {
            const std::string sourceKeyString = sourceCacheKey(sourceKey);
            auto cached =
                asyncState_->sourceTileDepotCache.find(sourceKeyString);
            if (cached != asyncState_->sourceTileDepotCache.end() &&
                cached->second.image) {
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
        asyncState_->inFlightRequests.insert(ck);
    }

    std::shared_ptr<ProviderAsyncState> state = asyncState_;
    std::weak_ptr<RasterOverlayTile> tileWeak;
    auto tileIt = tiles_.find(ck);
    if (tileIt != tiles_.end()) {
        tileWeak = tileIt->second;
    }
    const Rectangle outputBounds = tile.getRectangle();
    const Rectangle targetBounds =
        unprojectProviderToGeographic(outputBounds, projection_);
    auto request = std::make_shared<RectangleSourceRequest>(
        provider_,
        scheme_,
        state,
        std::move(sourcePlan),
        targetBounds,
        outputBounds,
        maximumCombinedTextureSize(textureUploader_.get(), maximumTextureSize_),
        getMinimumLevel(),
        getMaximumLevel(),
        [state, ck, tileWeak](std::unique_ptr<DecodedImage> image,
                              Rectangle rectangle,
                              RasterOverlayTile::MoreDetailAvailable moreDetailAvailable) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->inFlightRequests.erase(ck);
            if (image) {
                if (!state->alive.load(std::memory_order_acquire)) {
                    return;
                }
                state->pendingUploads.push_back(
                    {ck, std::move(image), rectangle, moreDetailAvailable});
            } else {
                if (auto tile = tileWeak.lock()) {
                    tile->setMoreDetailAvailable(
                        RasterOverlayTile::MoreDetailAvailable::No);
                    tile->setState(RasterOverlayTile::LoadState::Failed);
                }
                auto& fr = state->failedTiles[ck];
                if (fr.firstFailTime == 0.0) {
                    fr.firstFailTime = std::chrono::duration<double>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                }
                fr.retries++;
                state->revision.fetch_add(1, std::memory_order_relaxed);
            }
        },
        [state, ck, tileWeak]() {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->inFlightRequests.erase(ck);
            if (auto tile = tileWeak.lock()) {
                tile->setMoreDetailAvailable(
                    RasterOverlayTile::MoreDetailAvailable::No);
                tile->setState(RasterOverlayTile::LoadState::Failed);
            }
            auto& fr = state->failedTiles[ck];
            if (fr.firstFailTime == 0.0) {
                fr.firstFailTime = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            }
            fr.retries++;
            state->revision.fetch_add(1, std::memory_order_relaxed);
        });

    request->issueAll(
        [state]() {
            state->activeRasterSourceRequests.fetch_add(
                1,
                std::memory_order_relaxed);
        },
        [state]() {
            uint32_t current = state->activeRasterSourceRequests.load(
                std::memory_order_relaxed);
            while (current > 0 &&
                   !state->activeRasterSourceRequests.compare_exchange_weak(
                       current,
                       current - 1,
                       std::memory_order_relaxed)) {}
        });

    return true;
}

bool RasterOverlayTileProvider::loadTileThrottled(RasterOverlayTile& tile,
                                                  FrameResourceBudget* budget) {
    // cesium-native: loadTileThrottled only starts Unloaded tiles. Once a
    // rectangle tile is Loading, its source dependencies are already attached
    // to provider-level source tile assets and do not need per-frame pumping.
    if (tile.getState() != RasterOverlayTile::LoadState::Unloaded) {
        return true;
    }

    if (getThrottledTilesCurrentlyLoading() >= maximumSimultaneousTileLoads) {
        return false;  // Throttled
    }

    return loadTile(tile, budget);
}

bool RasterOverlayTileProvider::loadRectangleTile(RasterOverlayTile& tile,
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
    {
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        if (asyncState_->inFlightRequests.count(ck)) return true;
    }

    const Rectangle outputBounds = tile.getRectangle();
    const Rectangle targetBounds =
        unprojectProviderToGeographic(outputBounds, projection_);
    const Rectangle sourceBounds =
        mapGeometryBoundsToImageryCoverage(targetBounds, coverageRectangle_);

    RectangleSourcePlan sourcePlan = buildRectangleSourcePlan(
        scheme_,
        provider_,
        textureUploader_.get(),
        targetBounds,
        sourceBounds,
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
    auto estimateNewSourceRequests = [&]() {
        int estimated = 0;
        std::lock_guard<std::mutex> lock(asyncState_->mutex);
        for (const TileKey& sourceKey : sourcePlan.sourceKeys) {
            const std::string sourceKeyString = sourceCacheKey(sourceKey);
            auto cached =
                asyncState_->sourceTileDepotCache.find(sourceKeyString);
            if (cached != asyncState_->sourceTileDepotCache.end() &&
                cached->second.image) {
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
        asyncState_->inFlightRequests.insert(ck);
    }
    logAndroidRasterPipeline(
        "start",
        ck,
        static_cast<int>(sourcePlan.sourceKeys.size()),
        sourcePlan.sourceZoom);

    std::shared_ptr<ProviderAsyncState> state = asyncState_;
    std::weak_ptr<RasterOverlayTile> tileWeak;
    auto tileIt = tiles_.find(ck);
    if (tileIt != tiles_.end()) {
        tileWeak = tileIt->second;
    }
    const int maxTextureSize =
        maximumCombinedTextureSize(textureUploader_.get(), maximumTextureSize_);
    auto request = std::make_shared<RectangleSourceRequest>(
        provider_,
        scheme_,
        state,
        sourcePlan,
        sourceBounds,
        outputBounds,
        maxTextureSize,
        getMinimumLevel(),
        getMaximumLevel(),
        [state, ck](std::unique_ptr<DecodedImage> composed,
                    Rectangle rectangle,
                    RasterOverlayTile::MoreDetailAvailable moreDetailAvailable) {
            std::lock_guard<std::mutex> providerLock(state->mutex);
            state->inFlightRequests.erase(ck);
            if (!state->alive.load(std::memory_order_acquire)) {
                return;
            }
            logAndroidRasterPipeline("composed", ck, 0, 0);
            state->pendingUploads.push_back(
                {ck, std::move(composed), rectangle, moreDetailAvailable});
        },
        [state, ck, tileWeak]() {
            std::lock_guard<std::mutex> providerLock(state->mutex);
            state->inFlightRequests.erase(ck);
            logAndroidRasterPipeline("compose-failed", ck, 0, 0);
            if (auto tile = tileWeak.lock()) {
                tile->setMoreDetailAvailable(
                    RasterOverlayTile::MoreDetailAvailable::No);
                tile->setState(RasterOverlayTile::LoadState::Failed);
            }
            auto& fr = state->failedTiles[ck];
            if (fr.firstFailTime == 0.0) {
                fr.firstFailTime = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            }
            fr.retries++;
            state->revision.fetch_add(1, std::memory_order_relaxed);
        });

    request->issueAll(
        [state]() {
            state->activeRasterSourceRequests.fetch_add(
                1,
                std::memory_order_relaxed);
        },
        [state]() {
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
            // Rectangle raster tiles can be 512x512+ and state finalization
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
                        return uploadAllowedDuringInteraction(
                            candidate.cacheKey,
                            candidate.image.get());
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

        const bool emptyImage =
            upload.image &&
            upload.image->width == 0 &&
            upload.image->height == 0 &&
            upload.image->channels == 0 &&
            upload.image->pixels.empty();
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

        if (!upload.image || !isDecodedImageUploadable(*upload.image)) {
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
            // Resource-prep upload (main-thread safe). Rectangle images are
            // already combined at the selector's target screen-pixel density;
            // on mobile, generating mipmaps for every rectangle image is
            // expensive main-thread work without improving the current
            // selected tile.
            const bool generateMipmaps = !tile.isRectangleTile();
            RasterTextureUploadOptions uploadOptions;
            uploadOptions.generateMipmaps = generateMipmaps;
            auto tex = textureUploader_
                ? textureUploader_->uploadRasterTexture(
                      *upload.image,
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
                tile.isRectangleTile() ? tile.getSourceZoom() : tile.getTileID().z;
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
                upload.image->width > 1024 ||
                upload.image->height > 1024) {
                __android_log_print(ANDROID_LOG_INFO, "RasterOverlayTileProvider",
                    "upload %.2fms size=%dx%d rectangle=%d mipmap=%d cache=%s",
                    uploadMs,
                    upload.image->width,
                    upload.image->height,
                    tile.isRectangleTile() ? 1 : 0,
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
