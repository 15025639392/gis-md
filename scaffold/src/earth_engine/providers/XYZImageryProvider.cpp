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
#include <memory>
#include <mutex>
#include <sstream>

namespace earth_engine {

namespace {

struct RequestCompletionGuard {
    std::atomic<int>& completed;
    ~RequestCompletionGuard() {
        completed.fetch_add(1, std::memory_order_relaxed);
    }
};

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
    if (!openGlobusGroupedY_) return key.schemeId == "XYZ-WebMercator";

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
    std::string url = urlTemplate_;
    auto replace = [&url](const std::string& ph, const std::string& val) {
        size_t pos = 0;
        while ((pos = url.find(ph, pos)) != std::string::npos) {
            url.replace(pos, ph.length(), val);
            pos += val.length();
        }
    };
    replace("{z}", std::to_string(providerKey.z));
    replace("{x}", std::to_string(providerKey.x));
    replace("{y}", std::to_string(providerKey.y));
    replace("{groupedY}", std::to_string(key.y));
    if (url.find("{tileGroup}") != std::string::npos) {
        std::string group = "mercator";
        const int tilesAtZoom = 1 << key.z;
        if (key.y >= 2 * tilesAtZoom) group = "south";
        else if (key.y >= tilesAtZoom) group = "north";
        replace("{tileGroup}", group);
    }
    if (url.find("{s}") != std::string::npos) {
        // 高德等使用 1-4 子域；OSM 等使用 0-3
        // 简单规则：如果模板中有 "0{s}" 则在前面补 0 的基础上用 1-4
        bool hasLeadingZero = (url.find("0{s}") != std::string::npos);
        int s = (providerKey.x + providerKey.y) % 4;
        if (hasLeadingZero) s += 1;  // 1-4 range
        replace("{s}", std::to_string(s));
    }
    return url;
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
                     tokenPtr,
                     callbackPtr,
                     statusCode,
                     bodyPtr]() mutable {
                        RequestCompletionGuard completion{requestsCompleted_};
                        if (tokenPtr->isCancelled() ||
                            statusCode != 200 ||
                            bodyPtr->empty()) {
                            (*callbackPtr)(key, nullptr);
                            return;
                        }
                        auto image =
                            decodeTile(bodyPtr->data(), bodyPtr->size());
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
                 tokenPtr,
                 callbackPtr,
                 statusCode,
                 bodyPtr]() mutable {
                    RequestCompletionGuard completion{requestsCompleted_};
                    if (tokenPtr->isCancelled() ||
                        statusCode != 200 ||
                        bodyPtr->empty()) {
                        (*callbackPtr)(key, nullptr);
                        return;
                    }
                    auto image = decodeTile(bodyPtr->data(), bodyPtr->size());
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
