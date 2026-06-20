#include "XYZImageryProvider.h"
#include "../core/async/AsyncSystem.h"
#include "../platform/bridge/CurlMultiRequestScheduler.h"
#include "../platform/bridge/PlatformBridge.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

#if __has_include(<stb_image.h>)
#include <stb_image.h>
#define EARTH_ENGINE_HAS_STB_IMAGE 1
#else
#define EARTH_ENGINE_HAS_STB_IMAGE 0
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cctype>
#include <memory>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace earth_engine {

namespace {

struct RequestCompletionGuard {
    std::atomic<int>& completed;
    ~RequestCompletionGuard() {
        completed.fetch_add(1, std::memory_order_relaxed);
    }
};

#ifdef __ANDROID__
constexpr int kMaxAndroidFailureLogs = 24;

void logAndroidXyzFailure(const char* stage,
                          const TileKey& key,
                          int statusCode,
                          size_t bodySize,
                          const std::string& url) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) >=
        kMaxAndroidFailureLogs) {
        return;
    }
    __android_log_print(
        ANDROID_LOG_WARN,
        "XYZImagery",
        "%s failed z=%d x=%d y=%d status=%d bytes=%zu url=%s",
        stage,
        key.z,
        key.x,
        key.y,
        statusCode,
        bodySize,
        url.c_str());
}
#else
void logAndroidXyzFailure(const char*,
                          const TileKey&,
                          int,
                          size_t,
                          const std::string&) {}
#endif

int64_t xTileCountForScheme(const std::string& schemeId, int z) {
    if (z < 0 || z >= 62) return 0;
    if (schemeId == "Geographic-TMS") {
        return int64_t{1} << (z + 1);
    }
    return int64_t{1} << z;
}

int64_t yTileCountForScheme(const std::string&, int z) {
    if (z < 0 || z >= 62) return 0;
    return int64_t{1} << z;
}

double radiansToDegrees(double radians) {
    return radians * 180.0 / 3.14159265358979323846264338327950288;
}

double degreesToRadians(double degrees) {
    return degrees * 3.14159265358979323846264338327950288 / 180.0;
}

struct TileDegreesRectangle {
    double west = 0.0;
    double south = 0.0;
    double east = 0.0;
    double north = 0.0;
};

struct TileProjectedRectangle {
    double minimumX = 0.0;
    double minimumY = 0.0;
    double maximumX = 0.0;
    double maximumY = 0.0;
};

double webMercatorFractionToLatitudeDegrees(double fraction) {
    const double mercatorAngle =
        3.14159265358979323846264338327950288 -
        2.0 * 3.14159265358979323846264338327950288 * fraction;
    return radiansToDegrees(std::atan(std::sinh(mercatorAngle)));
}

double webMercatorLatitudeDegreesToProjectedY(double latitudeDegrees) {
    constexpr double kWgs84MaximumRadius = 6378137.0;
    const double latitude = degreesToRadians(latitudeDegrees);
    const double sinLatitude = std::sin(latitude);
    return 0.5 * std::log((1.0 + sinLatitude) / (1.0 - sinLatitude)) *
           kWgs84MaximumRadius;
}

TileDegreesRectangle tileDegreesRectangleForScheme(const TileKey& key) {
    const int64_t xTiles = xTileCountForScheme(key.schemeId, key.z);
    const int64_t yTiles = yTileCountForScheme(key.schemeId, key.z);
    if (xTiles <= 0 || yTiles <= 0) return {};

    const double west =
        static_cast<double>(key.x) / static_cast<double>(xTiles) * 360.0 -
        180.0;
    const double east =
        static_cast<double>(key.x + 1) / static_cast<double>(xTiles) * 360.0 -
        180.0;

    if (key.schemeId == "Geographic-TMS") {
        const double south =
            -90.0 +
            static_cast<double>(key.y) / static_cast<double>(yTiles) * 180.0;
        const double north =
            -90.0 +
            static_cast<double>(key.y + 1) / static_cast<double>(yTiles) *
                180.0;
        return TileDegreesRectangle{west, south, east, north};
    }

    if (key.schemeId == "TMS-WebMercator") {
        const double northFraction =
            static_cast<double>(yTiles - key.y - 1) /
            static_cast<double>(yTiles);
        const double southFraction =
            static_cast<double>(yTiles - key.y) / static_cast<double>(yTiles);
        return TileDegreesRectangle{
            west,
            webMercatorFractionToLatitudeDegrees(southFraction),
            east,
            webMercatorFractionToLatitudeDegrees(northFraction)};
    }

    const double northFraction =
        static_cast<double>(key.y) / static_cast<double>(yTiles);
    const double southFraction =
        static_cast<double>(key.y + 1) / static_cast<double>(yTiles);
    return TileDegreesRectangle{
        west,
        webMercatorFractionToLatitudeDegrees(southFraction),
        east,
        webMercatorFractionToLatitudeDegrees(northFraction)};
}

TileProjectedRectangle tileProjectedRectangleForScheme(const TileKey& key) {
    const TileDegreesRectangle degrees = tileDegreesRectangleForScheme(key);
    if (key.schemeId == "Geographic-TMS") {
        return TileProjectedRectangle{
            degreesToRadians(degrees.west),
            degreesToRadians(degrees.south),
            degreesToRadians(degrees.east),
            degreesToRadians(degrees.north)};
    }

    constexpr double kWgs84MaximumRadius = 6378137.0;
    return TileProjectedRectangle{
        degreesToRadians(degrees.west) * kWgs84MaximumRadius,
        webMercatorLatitudeDegreesToProjectedY(degrees.south),
        degreesToRadians(degrees.east) * kWgs84MaximumRadius,
        webMercatorLatitudeDegreesToProjectedY(degrees.north)};
}

std::string asciiLower(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string substituteUrlTemplateParameters(
    const std::string& templateUrl,
    const std::vector<std::pair<std::string, std::string>>& placeholders) {
    std::string result;
    size_t startPos = 0;
    while (true) {
        const size_t open = templateUrl.find('{', startPos);
        if (open == std::string::npos) break;
        result.append(templateUrl, startPos, open - startPos);

        const size_t close = templateUrl.find('}', open + 1);
        if (close == std::string::npos) {
            startPos = open;
            break;
        }

        const std::string placeholder =
            asciiLower(templateUrl.substr(open + 1, close - open - 1));
        bool replaced = false;
        for (const auto& [name, value] : placeholders) {
            if (name == placeholder) {
                result.append(value);
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            result.append("[UNKNOWN PLACEHOLDER]");
        }
        startPos = close + 1;
    }
    result.append(templateUrl, startPos, templateUrl.length() - startPos);
    return result;
}

} // namespace

// ============================================================
// XYZImageryProvider
// ============================================================

XYZImageryProvider::XYZImageryProvider(std::string urlTemplate,
                                         std::string attribution)
    : urlTemplate_(std::move(urlTemplate)),
      attribution_(std::move(attribution)) {}

XYZImageryProvider::~XYZImageryProvider() = default;

void XYZImageryProvider::setPlatformBridge(PlatformBridge* bridge) {
    platformBridge_ = bridge;
}

std::string XYZImageryProvider::id() const {
    std::ostringstream oss;
    oss << "xyz-" << std::hash<std::string>{}(urlTemplate_);
    return oss.str();
}

void XYZImageryProvider::setZoomRange(int minZoom, int maxZoom) {
    minZoom_ = minZoom;
    maxZoom_ = maxZoom;
}

void XYZImageryProvider::setTileSize(int width, int height) {
    tileWidth_ = width;
    tileHeight_ = height;
}

void XYZImageryProvider::setSchemeId(std::string schemeId) {
    schemeId_ = std::move(schemeId);
}

void XYZImageryProvider::setOpenGlobusGroupedY(bool enabled) {
    openGlobusGroupedY_ = enabled;
    if (enabled) {
        schemeId_ = "OpenGlobus-Earth";
    }
}

void XYZImageryProvider::setOpenGlobusPolarGroupsEnabled(bool enabled) {
    openGlobusPolarGroupsEnabled_ = enabled;
}

bool XYZImageryProvider::supportsTile(const TileKey& key) const {
    if (key.z < minZoom_ || key.z > maxZoom_) return false;
    if (key.schemeId != schemeId_) return false;
    if (!openGlobusGroupedY_) {
        const int64_t xTiles = xTileCountForScheme(key.schemeId, key.z);
        const int64_t yTiles = yTileCountForScheme(key.schemeId, key.z);
        return xTiles > 0 && yTiles > 0 &&
               key.x >= 0 && static_cast<int64_t>(key.x) < xTiles &&
               key.y >= 0 && static_cast<int64_t>(key.y) < yTiles;
    }

    const int tilesAtZoom = 1 << key.z;
    if (key.y < 0 || key.y >= 3 * tilesAtZoom) return false;
    if (!openGlobusPolarGroupsEnabled_ && key.y >= tilesAtZoom) return false;
    return true;
}

TileKey XYZImageryProvider::providerKeyForTile(const TileKey& key) const {
    if (!openGlobusGroupedY_) return key;

    const int tilesAtZoom = 1 << key.z;
    TileKey mapped = key;
    if (mapped.y >= 2 * tilesAtZoom) {
        mapped.y -= 2 * tilesAtZoom;
    } else if (mapped.y >= tilesAtZoom) {
        mapped.y -= tilesAtZoom;
    }
    return mapped;
}

std::string XYZImageryProvider::buildUrl(const TileKey& key) const {
    const TileKey providerKey = providerKeyForTile(key);
    const int64_t xTiles = xTileCountForScheme(providerKey.schemeId,
                                               providerKey.z);
    const int64_t yTiles = yTileCountForScheme(providerKey.schemeId,
                                               providerKey.z);
    const TileDegreesRectangle tileRect =
        tileDegreesRectangleForScheme(providerKey);
    const TileProjectedRectangle projectedRect =
        tileProjectedRectangleForScheme(providerKey);

    std::string group = "mercator";
    const int tilesAtZoom = 1 << key.z;
    if (key.y >= 2 * tilesAtZoom) group = "south";
    else if (key.y >= tilesAtZoom) group = "north";

    int subdomain = (providerKey.x + providerKey.y) % 4;
    if (urlTemplate_.find("0{s}") != std::string::npos) {
        subdomain += 1;
    }

    std::vector<std::pair<std::string, std::string>> placeholders{
        {"z", std::to_string(providerKey.z)},
        {"x", std::to_string(providerKey.x)},
        {"y", std::to_string(providerKey.y)},
        {"reversez", std::to_string(maxZoom_ - providerKey.z)},
        {"westdegrees", std::to_string(tileRect.west)},
        {"southdegrees", std::to_string(tileRect.south)},
        {"eastdegrees", std::to_string(tileRect.east)},
        {"northdegrees", std::to_string(tileRect.north)},
        {"minimumx", std::to_string(projectedRect.minimumX)},
        {"minimumy", std::to_string(projectedRect.minimumY)},
        {"maximumx", std::to_string(projectedRect.maximumX)},
        {"maximumy", std::to_string(projectedRect.maximumY)},
        {"width", std::to_string(tileWidth_)},
        {"height", std::to_string(tileHeight_)},
        {"groupedy", std::to_string(key.y)},
        {"tilegroup", group},
        {"s", std::to_string(subdomain)}};
    if (xTiles > 0) {
        placeholders.emplace_back("reversex",
                                  std::to_string(xTiles - 1 - providerKey.x));
    }
    if (yTiles > 0) {
        placeholders.emplace_back("reversey",
                                  std::to_string(yTiles - 1 - providerKey.y));
    }
    return substituteUrlTemplateParameters(urlTemplate_, placeholders);
}

void XYZImageryProvider::requestTile(const TileKey& key,
                                      CancellationToken token,
                                      TileCallback callback,
                                      HttpRequestPriority priority) {
    std::string url = buildUrl(key);
    if (platformBridge_) {
        requestsStarted_.fetch_add(1, std::memory_order_relaxed);
        if (token.isCancelled()) {
            requestsCompleted_.fetch_add(1, std::memory_order_relaxed);
            callback(key, nullptr);
            return;
        }

        auto requestHandle =
            std::make_shared<std::unique_ptr<HttpRequest>>();
        *requestHandle = platformBridge_->get(
            url,
            [this,
             key,
             url,
             token = std::move(token),
             callback = std::move(callback),
             requestHandle](int statusCode, std::vector<uint8_t> body) mutable {
                (void)requestHandle;
                auto tokenPtr =
                    std::make_shared<CancellationToken>(std::move(token));
                auto callbackPtr =
                    std::make_shared<TileCallback>(std::move(callback));
                auto bodyPtr =
                    std::make_shared<std::vector<uint8_t>>(std::move(body));
                AsyncSystem::run(
                    [this,
                     key,
                     url,
                     tokenPtr,
                     callbackPtr,
                     statusCode,
                     bodyPtr]() mutable {
                        RequestCompletionGuard completion{requestsCompleted_};
                        if (tokenPtr->isCancelled() ||
                            statusCode != 200 ||
                            bodyPtr->empty()) {
                            if (!tokenPtr->isCancelled()) {
                                logAndroidXyzFailure(
                                    "http",
                                    key,
                                    statusCode,
                                    bodyPtr->size(),
                                    url);
                            }
                            (*callbackPtr)(key, nullptr);
                            return;
                        }
                        auto image =
                            decodeTile(bodyPtr->data(), bodyPtr->size());
                        if (!image) {
                            logAndroidXyzFailure(
                                "decode",
                                key,
                                statusCode,
                                bodyPtr->size(),
                                url);
                        }
                        (*callbackPtr)(key, std::move(image));
                    });
            },
            {priority});
        return;
    }

    requestsStarted_.fetch_add(1, std::memory_order_relaxed);
    if (token.isCancelled()) {
        requestsCompleted_.fetch_add(1, std::memory_order_relaxed);
        callback(key, nullptr);
        return;
    }

    auto requestHandle =
        std::make_shared<std::unique_ptr<HttpRequest>>();
    *requestHandle = CurlMultiRequestScheduler::shared().get(
        url,
         [this,
          key,
          url,
          token = std::move(token),
          callback = std::move(callback),
          requestHandle](int statusCode, std::vector<uint8_t> body) mutable {
            (void)requestHandle;
            auto tokenPtr =
                std::make_shared<CancellationToken>(std::move(token));
            auto callbackPtr =
                std::make_shared<TileCallback>(std::move(callback));
            auto bodyPtr =
                std::make_shared<std::vector<uint8_t>>(std::move(body));
            AsyncSystem::run(
                [this,
                 key,
                 url,
                 tokenPtr,
                 callbackPtr,
                 statusCode,
                 bodyPtr]() mutable {
                    RequestCompletionGuard completion{requestsCompleted_};
                    if (tokenPtr->isCancelled() ||
                        statusCode != 200 ||
                        bodyPtr->empty()) {
                        if (!tokenPtr->isCancelled()) {
                            logAndroidXyzFailure(
                                "http",
                                key,
                                statusCode,
                                bodyPtr->size(),
                                url);
                        }
                        (*callbackPtr)(key, nullptr);
                        return;
                    }
                    auto image = decodeTile(bodyPtr->data(), bodyPtr->size());
                    if (!image) {
                        logAndroidXyzFailure(
                            "decode",
                            key,
                            statusCode,
                            bodyPtr->size(),
                            url);
                    }
                    (*callbackPtr)(key, std::move(image));
                });
        },
        {priority});
}

ProviderRequestDiagnostics XYZImageryProvider::requestDiagnostics() const {
    ProviderRequestDiagnostics diag;
    diag.requestsStarted = requestsStarted_.load(std::memory_order_relaxed);
    diag.requestsCompleted =
        requestsCompleted_.load(std::memory_order_relaxed);
    diag.activeWorkerBlockingRequests =
        activeWorkerBlockingRequests_.load(std::memory_order_relaxed);
    diag.peakWorkerBlockingRequests =
        peakWorkerBlockingRequests_.load(std::memory_order_relaxed);
    diag.maximumTransportActiveRequests = platformBridge_
        ? platformBridge_->maximumActiveRequests()
        : CurlMultiRequestScheduler::shared().maximumActiveRequests();
    return diag;
}

std::unique_ptr<DecodedImage> XYZImageryProvider::decodeTile(
    const uint8_t* data, size_t len) {
    // 优先使用 PlatformBridge 解码（Android JNI 侧有 stb_image 或平台解码器）
    if (platformBridge_) {
        return platformBridge_->decodeImage(data, len);
    }

#if EARTH_ENGINE_HAS_STB_IMAGE
    int w = 0, h = 0, c = 0;
    unsigned char* pixels = stbi_load_from_memory(
        data, static_cast<int>(len), &w, &h, &c, 4);
    if (!pixels) return nullptr;

    auto img = std::make_unique<DecodedImage>();
    img->width = w;
    img->height = h;
    img->channels = 4;
    img->pixels.assign(pixels, pixels + static_cast<size_t>(w * h * 4));
    stbi_image_free(pixels);
    return img;
#else
    (void)data;
    (void)len;
    return nullptr;
#endif
}

} // namespace earth_engine
