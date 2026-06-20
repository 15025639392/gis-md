#include "RasterOverlayTileProvider.h"
#include "ImageryProvider.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../tiling/TileScheme.h"
#include "RasterTextureUploader.h"
#include "../renderer/RenderDevice.h"
#include "../threading/CancellationToken.h"
#include "../debug/PerfTimer.h"

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

int maximumCombinedTextureSize(const RasterTextureUploader* uploader) {
    if (!uploader) return kMaximumCombinedTextureSizeFallback;
    const int backendMaxTextureSize = uploader->maxTextureSize();
    if (backendMaxTextureSize <= 0) {
        return kMaximumCombinedTextureSizeFallback;
    }
    return std::max(1, std::min(backendMaxTextureSize,
                                kMaximumCombinedTextureSizeFallback));
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
                                       const Rectangle& bounds,
                                       int zoom,
                                       TileRange range) {
    if (range.maxX < range.minX || range.maxY < range.minY) {
        return range;
    }

    // cesium-native QuadtreeRasterOverlayTileProvider excludes tiles that only
    // touch a geometry rectangle along a tile edge, using 1/512 of the geometry
    // span as the edge tolerance.
    const double veryCloseX = std::max(1e-12, bounds.width()) / 512.0;
    const double veryCloseY = std::max(1e-12, bounds.height()) / 512.0;

    const Rectangle westTile = scheme.tileToRectangle(
        TileKey{scheme.id(), zoom, range.minX, range.minY});
    if (std::abs(westTile.east() - bounds.west()) < veryCloseX &&
        range.minX < range.maxX) {
        ++range.minX;
    }

    const Rectangle eastTile = scheme.tileToRectangle(
        TileKey{scheme.id(), zoom, range.maxX, range.maxY});
    if (std::abs(eastTile.west() - bounds.east()) < veryCloseX &&
        range.maxX > range.minX) {
        --range.maxX;
    }

    const bool yDown = scheme.yDirection().find("down") != std::string::npos;
    if (yDown) {
        const Rectangle northTile = scheme.tileToRectangle(
            TileKey{scheme.id(), zoom, range.minX, range.minY});
        if (std::abs(northTile.south() - bounds.north()) < veryCloseY &&
            range.minY < range.maxY) {
            ++range.minY;
        }

        const Rectangle southTile = scheme.tileToRectangle(
            TileKey{scheme.id(), zoom, range.maxX, range.maxY});
        if (std::abs(southTile.north() - bounds.south()) < veryCloseY &&
            range.maxY > range.minY) {
            --range.maxY;
        }
    } else {
        const Rectangle southTile = scheme.tileToRectangle(
            TileKey{scheme.id(), zoom, range.minX, range.minY});
        if (std::abs(southTile.north() - bounds.south()) < veryCloseY &&
            range.minY < range.maxY) {
            ++range.minY;
        }

        const Rectangle northTile = scheme.tileToRectangle(
            TileKey{scheme.id(), zoom, range.maxX, range.maxY});
        if (std::abs(northTile.south() - bounds.north()) < veryCloseY &&
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

bool canIssueRasterRequestBudget(FrameResourceBudget* budget,
                                 uint32_t currentInflight,
                                 int estimatedFanout) {
    if (!budget) {
        return true;
    }
    return budget->hasNetworkInflightCapacity(
               FrameResourceLane::RasterRequest,
               currentInflight,
               estimatedFanout) &&
           budget->canIssue(
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

double webMercatorY(double latRad) {
    const double lat = std::clamp(
        latRad, -kMaxWebMercatorLat, kMaxWebMercatorLat);
    return std::log(std::tan(lat * 0.5 + kPi * 0.25));
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

double latitudeAtProjectedV(const TileScheme& scheme,
                            const Rectangle& bounds,
                            double v) {
    if (!isWebMercatorScheme(scheme)) {
        return bounds.north() - v * bounds.height();
    }

    const double north = projectedNorth(scheme, bounds);
    const double south = projectedSouth(scheme, bounds);
    const double projected = north - v * std::abs(north - south);
    return std::atan(std::sinh(projected));
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
                                       double maximumScreenSpaceError) {
    const int minZoom = std::max(scheme.minZoom(), provider.minZoom());
    const int maxZoom = std::min(scheme.maxZoom(), provider.maxZoom());
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
                        const Rectangle& bounds,
                        double targetScreenPixelsX,
                        double targetScreenPixelsY,
                        double maximumScreenSpaceError,
                        TileRange* outRange = nullptr) {
    const int minZoom = std::max(scheme.minZoom(), provider.minZoom());
    const int maxZoom = std::min(scheme.maxZoom(), provider.maxZoom());
    if (maxZoom < minZoom) {
        if (outRange) *outRange = TileRange{};
        return scheme.minZoom();
    }

    int zoom = computeLevelFromTargetScreenPixels(
        scheme,
        provider,
        bounds,
        targetScreenPixelsX,
        targetScreenPixelsY,
        maximumScreenSpaceError);
    const int maxTextureSize = maximumCombinedTextureSize(uploader);

    TileRange range = trimCesiumNativeBoundarySlop(
        scheme, bounds, zoom, computeRange(scheme, bounds, zoom));
    while (zoom > minZoom) {
        const int widthPixels = range.width() * std::max(1, provider.tileWidth());
        const int heightPixels = range.height() * std::max(1, provider.tileHeight());
        if (widthPixels <= maxTextureSize &&
            heightPixels <= maxTextureSize) {
            break;
        }
        --zoom;
        range = trimCesiumNativeBoundarySlop(
            scheme, bounds, zoom, computeRange(scheme, bounds, zoom));
    }

    if (outRange) *outRange = range;
    return zoom;
}

RectangleSourcePlan buildRectangleSourcePlan(
    const TileScheme& scheme,
    const ImageryProvider& provider,
    const RasterTextureUploader* uploader,
    const Rectangle& bounds,
    double targetScreenPixelsX,
    double targetScreenPixelsY,
    double maximumScreenSpaceError) {
    RectangleSourcePlan plan;
    plan.sourceZoom = chooseRectangleSourceZoom(
        scheme,
        provider,
        uploader,
        bounds,
        targetScreenPixelsX,
        targetScreenPixelsY,
        maximumScreenSpaceError,
        &plan.range);
    plan.sourceKeys.reserve(
        static_cast<size_t>(std::max(0, plan.range.count())));
    for (int y = plan.range.minY; y <= plan.range.maxY; ++y) {
        for (int x = plan.range.minX; x <= plan.range.maxX; ++x) {
            TileKey sourceKey{scheme.id(), plan.sourceZoom, x, y};
            if (provider.supportsTile(sourceKey)) {
                plan.sourceKeys.push_back(sourceKey);
            }
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

TileKey parentTileKey(const TileKey& key) {
    return key.parent();
}

double clampUnit(double v) {
    return std::max(0.0, std::min(1.0, v));
}

struct LoadedSourceImage {
    TileKey key;
    Rectangle bounds;
    std::unique_ptr<DecodedImage> image;
};

const LoadedSourceImage* findSourceForPosition(
    const TileScheme& scheme,
    const std::unordered_map<TileKey, size_t>& sourceByKey,
    const std::vector<LoadedSourceImage>& sources,
    double lng,
    double lat,
    int sourceZoom) {
    TileKey key = scheme.positionToTile(lng, lat, sourceZoom);
    auto it = sourceByKey.find(key);
    if (it != sourceByKey.end() && it->second < sources.size()) {
        return &sources[it->second];
    }

    // Edge precision fallback. The center-of-pixel path should almost always
    // hit by key; this handles polar/edge rectangles conservatively.
    for (const auto& source : sources) {
        if (source.image && source.bounds.contains(lng, lat)) {
            return &source;
        }
    }
    return nullptr;
}

std::unique_ptr<DecodedImage> combineRectangleImages(
    const TileScheme& scheme,
    const Rectangle& targetBounds,
    int sourceZoom,
    std::vector<LoadedSourceImage>&& sources,
    int maximumTextureSize) {
    sources.erase(
        std::remove_if(sources.begin(), sources.end(),
                       [](const LoadedSourceImage& source) {
                           return !source.image || source.image->pixels.empty() ||
                                  source.image->width <= 0 ||
                                  source.image->height <= 0 ||
                                  source.image->channels < 3;
                       }),
        sources.end());
    if (sources.empty()) return nullptr;

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
        return nullptr;
    }

    int width = static_cast<int>(std::ceil(targetBounds.width() /
                                           projectedWidthPerPixel));
    int height = static_cast<int>(std::ceil(projectedHeight(scheme, targetBounds) /
                                            projectedHeightPerPixel));
    width = std::clamp(width, 1, maximumTextureSize);
    height = std::clamp(height, 1, maximumTextureSize);

    auto output = std::make_unique<DecodedImage>();
    output->width = width;
    output->height = height;
    output->channels = 4;
    output->pixels.resize(static_cast<size_t>(width) *
                          static_cast<size_t>(height) * 4u, 0);

    std::unordered_map<TileKey, size_t> sourceByKey;
    sourceByKey.reserve(sources.size());
    for (size_t i = 0; i < sources.size(); ++i) {
        sourceByKey[sources[i].key] = i;
    }

    int filledPixels = 0;
    for (int y = 0; y < height; ++y) {
        const double v = (static_cast<double>(y) + 0.5) /
                         static_cast<double>(height);
        const double lat = latitudeAtProjectedV(scheme, targetBounds, v);
        for (int x = 0; x < width; ++x) {
            const double u = (static_cast<double>(x) + 0.5) /
                             static_cast<double>(width);
            const double lng = targetBounds.west() +
                               u * targetBounds.width();

            const LoadedSourceImage* source = findSourceForPosition(
                scheme, sourceByKey, sources, lng, lat, sourceZoom);
            if (!source || !source->image) continue;

            const DecodedImage& src = *source->image;
            const double su = clampUnit(
                (lng - source->bounds.west()) / source->bounds.width());
            const double sv = projectedVForLatitudeInternal(
                scheme,
                source->bounds,
                lat);
            const int sx = std::clamp(
                static_cast<int>(su * static_cast<double>(src.width)),
                0,
                src.width - 1);
            const int sy = std::clamp(
                static_cast<int>(sv * static_cast<double>(src.height)),
                0,
                src.height - 1);

            const size_t srcIndex =
                (static_cast<size_t>(sy) * static_cast<size_t>(src.width) +
                 static_cast<size_t>(sx)) *
                static_cast<size_t>(src.channels);
            const size_t dstIndex =
                (static_cast<size_t>(y) * static_cast<size_t>(width) +
                 static_cast<size_t>(x)) * 4u;

            output->pixels[dstIndex + 0] = src.pixels[srcIndex + 0];
            output->pixels[dstIndex + 1] = src.pixels[srcIndex + 1];
            output->pixels[dstIndex + 2] = src.pixels[srcIndex + 2];
            output->pixels[dstIndex + 3] =
                src.channels >= 4 ? src.pixels[srcIndex + 3] : 255;
            ++filledPixels;
        }
    }

    if (filledPixels != width * height) {
        return nullptr;
    }

    return output;
}

using RectangleRequestSuccess =
    std::function<void(std::unique_ptr<DecodedImage>)>;
using RectangleRequestFailure = std::function<void()>;

} // namespace

struct RasterOverlayTileProvider::RectangleSourceRequest
    : public std::enable_shared_from_this<RectangleSourceRequest> {
    RectangleSourceRequest(ImageryProvider& imageryProvider,
                           const TileScheme& tileScheme,
                           RectangleSourcePlan plan,
                           Rectangle bounds,
                           int textureSize,
                           RectangleRequestSuccess success,
                           RectangleRequestFailure failure)
        : provider(imageryProvider)
        , scheme(tileScheme)
        , sourcePlan(std::move(plan))
        , targetBounds(bounds)
        , maximumTextureSize(textureSize)
        , onSuccess(std::move(success))
        , onFailure(std::move(failure))
        , remaining(sourcePlan.budgetUnits()) {
        sources.reserve(sourcePlan.sourceKeys.size());
    }

    bool issueMore(FrameResourceBudget* budget,
                   const std::function<uint32_t()>& currentInflight,
                   const std::function<void()>& onSourceIssued,
                   const std::function<void()>& onSourceFinished) {
        bool issuedAny = false;
        while (!isComplete()) {
            TileKey sourceKey;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (nextSourceIndex >= sourcePlan.sourceKeys.size()) {
                    break;
                }
                sourceKey = sourcePlan.sourceKeys[nextSourceIndex];
            }

            if (!tryIssueRasterRequestBudget(
                    budget,
                    currentInflight(),
                    1)) {
                break;
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                if (nextSourceIndex >= sourcePlan.sourceKeys.size()) {
                    break;
                }
                sourceKey = sourcePlan.sourceKeys[nextSourceIndex++];
            }
            issuedAny = true;
            onSourceIssued();
            requestSource(sourceKey, onSourceFinished);
        }
        return issuedAny;
    }

    bool isComplete() const {
        std::lock_guard<std::mutex> lock(mutex);
        return completed;
    }

private:
    void requestSource(
        const TileKey& requestedKey,
        const std::function<void()>& onSourceFinished) {
        auto self = shared_from_this();
        CancellationToken token;
        provider.requestTile(
            requestedKey,
            token,
            [self, requestedKey, onSourceFinished](
                const TileKey& loadedKey,
                std::unique_ptr<DecodedImage> image) mutable {
                if (image) {
                    LoadedSourceImage source;
                    source.key = loadedKey;
                    source.bounds = self->scheme.tileToRectangle(loadedKey);
                    source.image = std::move(image);
                    onSourceFinished();
                    self->finishOneSource(std::move(source));
                    return;
                }

                // cesium-native QuadtreeRasterOverlayTileProvider:
                // failed sub-tiles try their parent before reporting an empty
                // contribution to the combined geometry image.
                if (requestedKey.z >
                    std::max(self->scheme.minZoom(),
                             self->provider.minZoom())) {
                    const TileKey parentKey = parentTileKey(requestedKey);
                    if (self->provider.supportsTile(parentKey)) {
                        self->requestSource(parentKey, onSourceFinished);
                        return;
                    }
                }

                onSourceFinished();
                self->finishOneSource(LoadedSourceImage{});
            });
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

        std::unique_ptr<DecodedImage> composed =
            combineRectangleImages(
                scheme,
                targetBounds,
                sourcePlan.sourceZoom,
                std::move(completedSources),
                maximumTextureSize);
        if (composed) {
            onSuccess(std::move(composed));
        } else {
            onFailure();
        }
    }

    ImageryProvider& provider;
    const TileScheme& scheme;
    RectangleSourcePlan sourcePlan;
    Rectangle targetBounds;
    int maximumTextureSize = 0;
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
    std::vector<LoadedSourceImage> sources;
    sources.reserve(publicSources.size());
    for (auto& source : publicSources) {
        sources.push_back(LoadedSourceImage{
            source.key,
            source.bounds,
            std::move(source.image)});
    }
    return combineRectangleImages(
        scheme,
        targetBounds,
        sourceZoom,
        std::move(sources),
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
    , textureUploader_(std::move(textureUploader)) {}

RasterOverlayTileProvider::~RasterOverlayTileProvider() = default;

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

    if (key.z < 0 || key.z > scheme_.maxZoom()) return nullptr;

    std::string ck = tileCacheKey(key);
    auto it = tiles_.find(ck);
    if (it != tiles_.end()) {
        it->second->lastUsedFrame = frameNumber_;
        return it->second;
    }

    // Create new tile in Unloaded state
    Rectangle bounds = scheme_.tileToRectangle(key);
    auto tile = std::make_shared<RasterOverlayTile>(*this, key, bounds, ck);
    tile->setMaxZoom(std::min(scheme_.maxZoom(), provider_.maxZoom()));
    tile->lastUsedFrame = frameNumber_;
    tiles_[ck] = tile;
    return tile;
}

RasterOverlayTileProvider::TilePtr RasterOverlayTileProvider::getTile(
    const Rectangle& geometryBounds,
    double targetScreenPixelsX,
    double targetScreenPixelsY) {
    // cesium-native: return placeholder if provider is not yet ready
    if (!ready_) {
        return getPlaceholderTile();
    }

    RectangleSourcePlan sourcePlan = buildRectangleSourcePlan(
        scheme_,
        provider_,
        textureUploader_.get(),
        geometryBounds,
        targetScreenPixelsX,
        targetScreenPixelsY,
        maximumScreenSpaceError_);

    const std::string ck = rectangleTileCacheKey(
        scheme_, geometryBounds, sourcePlan.sourceZoom);
    auto it = tiles_.find(ck);
    if (it != tiles_.end()) {
        it->second->lastUsedFrame = frameNumber_;
        return it->second;
    }

    const double centerLng =
        geometryBounds.west() + geometryBounds.width() * 0.5;
    const double centerLat =
        geometryBounds.south() + geometryBounds.height() * 0.5;
    TileKey representativeKey = scheme_.positionToTile(
        centerLng, centerLat, sourcePlan.sourceZoom);

    auto tile = std::make_shared<RasterOverlayTile>(
        *this, representativeKey, geometryBounds, ck);
    tile->setMaxZoom(std::min(scheme_.maxZoom(), provider_.maxZoom()));
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

    for (int z = desiredZoom; z >= scheme_.minZoom(); --z) {
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
    std::lock_guard<std::mutex> lock(pendingMutex_);
    return static_cast<int>(pendingUploads_.size());
}

bool RasterOverlayTileProvider::loadTile(RasterOverlayTile& tile,
                                         FrameResourceBudget* budget) {
    if (tile.isRectangleTile()) {
        return loadRectangleTile(tile, budget);
    }

    // cesium-native ActivatedRasterOverlay::doLoad: only Unloaded tiles start
    // a load. Loading/Loaded/Done/Failed/Placeholder are terminal or already
    // in progress from this entry point.
    auto state = tile.getState();
    switch (state) {
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
    if (inFlightRequests_.count(ck)) {
        return true;
    }
    if (!tryIssueRasterRequestBudget(
            budget,
            activeRasterSourceRequests_,
            1)) {
        return false;
    }

    // Mark as Loading
    tile.setState(RasterOverlayTile::LoadState::Loading);
    inFlightRequests_.insert(ck);
    ++activeRasterSourceRequests_;

    // cesium-native: delegate to imagery provider for async HTTP load
    CancellationToken token;
    auto* self = this;
    provider_.requestTile(key, token,
        [self, ck](const TileKey& /*k*/, std::unique_ptr<DecodedImage> image) {
            std::lock_guard<std::mutex> lock(self->pendingMutex_);
            if (self->activeRasterSourceRequests_ > 0) {
                --self->activeRasterSourceRequests_;
            }
            self->inFlightRequests_.erase(ck);
            if (image) {
                self->pendingUploads_.push_back({ck, std::move(image)});
            } else {
                // Mark as Failed
                auto it = self->tiles_.find(ck);
                if (it != self->tiles_.end()) {
                    it->second->setMoreDetailAvailable(
                        RasterOverlayTile::MoreDetailAvailable::No);
                    it->second->setState(RasterOverlayTile::LoadState::Failed);
                }
                auto& fr = self->failedTiles_[ck];
                if (fr.firstFailTime == 0.0) {
                    fr.firstFailTime = std::chrono::duration<double>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                }
                fr.retries++;
                self->revision_.fetch_add(1, std::memory_order_relaxed);
            }
        });

    return true;
}

bool RasterOverlayTileProvider::loadTileThrottled(RasterOverlayTile& tile,
                                                  FrameResourceBudget* budget) {
    if (tile.isRectangleTile() &&
        tile.getState() == RasterOverlayTile::LoadState::Loading) {
        return loadRectangleTile(tile, budget);
    }

    // cesium-native: loadTileThrottled only starts Unloaded tiles. Rectangle
    // tiles keep the Loading continuation above so source fanout can be issued
    // across frames under the local request budget.
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
    auto state = tile.getState();
    switch (state) {
        case RasterOverlayTile::LoadState::Unloaded:
            break;
        case RasterOverlayTile::LoadState::Loading: {
            const std::string ck = tile.getCacheKey();
            auto it = activeRectangleRequests_.find(ck);
            if (it == activeRectangleRequests_.end() || !it->second) {
                return true;
            }
            it->second->issueMore(
                budget,
                [self = this]() { return self->activeRasterSourceRequests_; },
                [self = this]() { ++self->activeRasterSourceRequests_; },
                [self = this]() {
                    if (self->activeRasterSourceRequests_ > 0) {
                        --self->activeRasterSourceRequests_;
                    }
                });
            return true;
        }
        case RasterOverlayTile::LoadState::Loaded:
        case RasterOverlayTile::LoadState::Done:
        case RasterOverlayTile::LoadState::Failed:
        case RasterOverlayTile::LoadState::Placeholder:
            return true;
    }

    const std::string ck = tile.getCacheKey();
    if (ck.empty()) return false;
    if (inFlightRequests_.count(ck)) return true;

    RectangleSourcePlan sourcePlan = buildRectangleSourcePlan(
        scheme_,
        provider_,
        textureUploader_.get(),
        tile.getRectangle(),
        tile.getTargetScreenPixelsX(),
        tile.getTargetScreenPixelsY(),
        maximumScreenSpaceError_);

    if (sourcePlan.empty()) {
        logAndroidRasterPipeline("empty-plan", ck, 0, sourcePlan.sourceZoom);
        tile.setMoreDetailAvailable(RasterOverlayTile::MoreDetailAvailable::No);
        tile.setState(RasterOverlayTile::LoadState::Failed);
        return false;
    }
    if (!canIssueRasterRequestBudget(
            budget,
            activeRasterSourceRequests_,
            1)) {
        return false;
    }

    tile.setState(RasterOverlayTile::LoadState::Loading);
    inFlightRequests_.insert(ck);
    logAndroidRasterPipeline(
        "start",
        ck,
        static_cast<int>(sourcePlan.sourceKeys.size()),
        sourcePlan.sourceZoom);

    auto* self = this;
    const Rectangle targetBounds = tile.getRectangle();
    const int maxTextureSize = maximumCombinedTextureSize(textureUploader_.get());
    auto request = std::make_shared<RectangleSourceRequest>(
        provider_,
        scheme_,
        sourcePlan,
        targetBounds,
        maxTextureSize,
        [self, ck](std::unique_ptr<DecodedImage> composed) {
            std::lock_guard<std::mutex> providerLock(self->pendingMutex_);
            self->inFlightRequests_.erase(ck);
            self->activeRectangleRequests_.erase(ck);
            logAndroidRasterPipeline("composed", ck, 0, 0);
            self->pendingUploads_.push_back({ck, std::move(composed)});
        },
        [self, ck]() {
            std::lock_guard<std::mutex> providerLock(self->pendingMutex_);
            self->inFlightRequests_.erase(ck);
            self->activeRectangleRequests_.erase(ck);
            logAndroidRasterPipeline("compose-failed", ck, 0, 0);
            auto it = self->tiles_.find(ck);
            if (it != self->tiles_.end()) {
                it->second->setMoreDetailAvailable(
                    RasterOverlayTile::MoreDetailAvailable::No);
                it->second->setState(RasterOverlayTile::LoadState::Failed);
            }
            auto& fr = self->failedTiles_[ck];
            if (fr.firstFailTime == 0.0) {
                fr.firstFailTime = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            }
            fr.retries++;
            self->revision_.fetch_add(1, std::memory_order_relaxed);
        });
    activeRectangleRequests_[ck] = request;
    request->issueMore(
        budget,
        [self = this]() { return self->activeRasterSourceRequests_; },
        [self = this]() { ++self->activeRasterSourceRequests_; },
        [self = this]() {
            if (self->activeRasterSourceRequests_ > 0) {
                --self->activeRasterSourceRequests_;
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
            std::lock_guard<std::mutex> lock(pendingMutex_);
            // Rectangle raster tiles can be 512x512+ and state finalization
            // runs on the main thread. Take one upload at a time so elapsed
            // upload cost can stop the next item in the same frame.
            if (pendingUploads_.empty()) {
                break;
            }
            if (!budget->tryFinalize(FrameResourceLane::RasterTextureUpload,
                                     FrameResourcePriority::Normal)) {
                break;
            }
            auto selected = pendingUploads_.begin();
            if (interactionActive) {
                selected = std::find_if(
                    pendingUploads_.begin(),
                    pendingUploads_.end(),
                    [](const PendingUpload& candidate) {
                        return uploadAllowedDuringInteraction(
                            candidate.cacheKey,
                            candidate.image.get());
                    });
                if (selected == pendingUploads_.end()) {
                    break;
                }
            }
            upload = std::move(*selected);
            pendingUploads_.erase(selected);
        }

        auto it = tiles_.find(upload.cacheKey);
        if (it == tiles_.end()) continue;

        RasterOverlayTile& tile = *it->second;
        if (!upload.image) {
            tile.setMoreDetailAvailable(RasterOverlayTile::MoreDetailAvailable::No);
            tile.setState(RasterOverlayTile::LoadState::Failed);
            revision_.fetch_add(1, std::memory_order_relaxed);
            ++processed;
            continue;
        }

        // Resource-prep upload (main-thread safe). Rectangle images are
        // already combined at the selector's target screen-pixel density; on
        // mobile, generating mipmaps for every rectangle image is expensive
        // main-thread work without improving the current selected tile.
        const bool generateMipmaps = !tile.isRectangleTile();
        const double uploadStartMs = perf::nowMs();
        RasterTextureUploadOptions uploadOptions;
        uploadOptions.generateMipmaps = generateMipmaps;
        auto tex = textureUploader_
            ? textureUploader_->uploadRasterTexture(*upload.image, uploadOptions)
            : nullptr;
        const double uploadMs = perf::nowMs() - uploadStartMs;
        budget->recordElapsed(FrameResourceLane::RasterTextureUpload, uploadMs);
#ifndef __ANDROID__
        (void)uploadMs;
#endif
        if (tex) {
            const int sourceLevel =
                tile.isRectangleTile() ? tile.getSourceZoom() : tile.getTileID().z;
            tile.setMoreDetailAvailable(
                sourceLevel < tile.getMaxZoom()
                    ? RasterOverlayTile::MoreDetailAvailable::Yes
                    : RasterOverlayTile::MoreDetailAvailable::No);
            // cesium-native: transfer texture ownership to the tile.
            // The tile owns its texture; no external cache needed.
            tile.setTexture(std::move(tex));
            revision_.fetch_add(1, std::memory_order_relaxed);
            ++processed;
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
        } else {
            tile.setMoreDetailAvailable(RasterOverlayTile::MoreDetailAvailable::No);
            tile.setState(RasterOverlayTile::LoadState::Failed);
            revision_.fetch_add(1, std::memory_order_relaxed);
            ++processed;
        }
    }
    return processed;
}

bool RasterOverlayTileProvider::hasPendingWork() const {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    return !pendingUploads_.empty() || !inFlightRequests_.empty();
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
        const bool inFlight = inFlightRequests_.count(it->first) > 0;
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
