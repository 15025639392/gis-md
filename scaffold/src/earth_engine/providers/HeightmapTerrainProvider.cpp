#include "HeightmapTerrainProvider.h"
#include "../core/async/AsyncSystem.h"
#include "../core/cache/HttpCache.h"
#include "../platform/bridge/PlatformBridge.h"

#ifndef EARTH_ENGINE_HAS_LIBCURL
#if !defined(ANDROID) && __has_include(<curl/curl.h>)
#include <curl/curl.h>
#define EARTH_ENGINE_HAS_LIBCURL 1
#else
#define EARTH_ENGINE_HAS_LIBCURL 0
#endif
#endif

#if __has_include(<stb_image.h>)
#include <stb_image.h>
#define EARTH_ENGINE_HAS_STB_IMAGE 1
#else
#define EARTH_ENGINE_HAS_STB_IMAGE 0
#endif

#include <sstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <cmath>
#include <fstream>

namespace earth_engine {

// ============================================================
// libcurl 辅助
// ============================================================

namespace {
#if EARTH_ENGINE_HAS_LIBCURL
static std::once_flag gCurlOnce;
static void ensureCurl() {
    std::call_once(gCurlOnce, []{ curl_global_init(CURL_GLOBAL_DEFAULT); });
}
static size_t curlWrite(void* p, size_t s, size_t n, void* u) {
    auto* b = static_cast<std::vector<uint8_t>*>(u);
    size_t t = s * n;
    b->insert(b->end(), static_cast<const uint8_t*>(p),
              static_cast<const uint8_t*>(p) + t);
    return t;
}
#endif
} // namespace

// ============================================================
// HeightmapTerrainProvider
// ============================================================

HeightmapTerrainProvider::HeightmapTerrainProvider(std::string urlTemplate,
                                                     std::string attribution)
    : urlTemplate_(std::move(urlTemplate)),
      attribution_(std::move(attribution)) {}

HeightmapTerrainProvider::~HeightmapTerrainProvider() = default;

void HeightmapTerrainProvider::setPlatformBridge(PlatformBridge* bridge) {
    platformBridge_ = bridge;
}

std::string HeightmapTerrainProvider::id() const {
    std::ostringstream oss;
    oss << "terrain-" << std::hash<std::string>{}(urlTemplate_);
    return oss.str();
}

void HeightmapTerrainProvider::setZoomRange(int minZ, int maxZ) {
    minZoom_ = minZ;
    maxZoom_ = maxZ;
}

void HeightmapTerrainProvider::setEncoding(Encoding encoding) {
    encoding_ = encoding;
}

std::string HeightmapTerrainProvider::buildUrl(const TileKey& key) const {
    std::string url = urlTemplate_;
    auto replace = [&url](const std::string& ph, const std::string& val) {
        size_t pos = 0;
        while ((pos = url.find(ph, pos)) != std::string::npos) {
            url.replace(pos, ph.length(), val);
            pos += val.length();
        }
    };
    replace("{z}", std::to_string(key.z));
    replace("{x}", std::to_string(key.x));
    replace("{y}", std::to_string(key.y));
    return url;
}

void HeightmapTerrainProvider::requestTile(const TileKey& key,
                                            CancellationToken token,
                                            HeightmapCallback callback) {
    std::string url = buildUrl(key);
    // cesium-native alignment: use thread pool instead of raw std::thread::detach().
    AsyncSystem::pool().enqueue(
        [this, url, key, token = std::move(token),
         callback = std::move(callback)]() mutable {
            if (token.isCancelled()) { callback(key, nullptr); return; }
            auto body = httpGet(url);
            if (token.isCancelled() || body.empty()) { callback(key, nullptr); return; }
            auto hm = decodeTile(body.data(), body.size());
            callback(key, std::move(hm));
        });
}

std::vector<uint8_t> HeightmapTerrainProvider::httpGet(const std::string& url) {
    // Shared LRU cache: avoid re-downloading recently fetched tiles.
    auto cached = HttpCache::shared().get(url);
    if (!cached.empty()) return cached;

    constexpr const char* kFilePrefix = "file://";
    if (url.rfind(kFilePrefix, 0) == 0) {
        std::ifstream in(url.substr(std::strlen(kFilePrefix)), std::ios::binary);
        if (!in) return {};
        std::vector<uint8_t> data{
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
        HttpCache::shared().put(url, data);
        return data;
    }

    // PlatformBridge 优先
    if (platformBridge_) {
        std::vector<uint8_t> result;
        std::mutex mtx;
        std::condition_variable cv;
        bool done = false;

        platformBridge_->get(url, [&](int code, std::vector<uint8_t> body) {
            if (code == 200) result = std::move(body);
            { std::lock_guard<std::mutex> lk(mtx); done = true; }
            cv.notify_one();
        });

        { std::unique_lock<std::mutex> lk(mtx);
          cv.wait_for(lk, std::chrono::seconds(20), [&]{ return done; }); }
        if (!result.empty()) HttpCache::shared().put(url, result);
        return result;
    }

    // libcurl 回退
#if EARTH_ENGINE_HAS_LIBCURL
    ensureCurl();
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::vector<uint8_t> body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "earth-md/0.1");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    std::vector<uint8_t> result =
        (res == CURLE_OK && httpCode == 200) ? std::move(body) : std::vector<uint8_t>{};
    if (!result.empty()) HttpCache::shared().put(url, result);
    return result;
#else
    (void)url;
    return {};
#endif
}

std::unique_ptr<DecodedHeightmap> HeightmapTerrainProvider::decodeTile(
    const uint8_t* data, size_t len) {
    // PlatformBridge decode 优先
    if (platformBridge_) {
        auto img = platformBridge_->decodeImage(data, len);
        if (!img || img->channels < 3) return nullptr;

        auto hm = std::make_unique<DecodedHeightmap>();
        hm->tileSize = img->width;
        hm->heightFactor = heightFactor_;
        hm->noDataValues = noDataValues_;

        size_t count = static_cast<size_t>(img->width * img->height);
        hm->heights.reserve(count);

        float minH = 1e30f, maxH = -1e30f;
        for (size_t i = 0; i < count; ++i) {
            size_t off = i * static_cast<size_t>(img->channels);
            float r = static_cast<float>(img->pixels[off]);
            float g = static_cast<float>(img->pixels[off + 1]);
            float b = static_cast<float>(img->pixels[off + 2]);
            float h = 0.0f;
            if (encoding_ == Encoding::MapboxTerrainRgb) {
                h = -10000.0f + (r * 65536.0f + g * 256.0f + b) * 0.1f;
            } else {
                h = r * 256.0f + g + b / 256.0f - 32768.0f;
            }
            h *= heightFactor_;  // OpenGlobus _heightFactor
            hm->heights.push_back(h);
            if (h < minH) minH = h;
            if (h > maxH) maxH = h;
        }
        hm->minHeight = minH;
        hm->maxHeight = maxH;
        return hm;
    }

#if EARTH_ENGINE_HAS_STB_IMAGE
    int w = 0, h = 0, c = 0;
    unsigned char* pixels = stbi_load_from_memory(
        data, static_cast<int>(len), &w, &h, &c, 3);
    if (!pixels) return nullptr;

    auto hm = std::make_unique<DecodedHeightmap>();
    hm->tileSize = w;
    hm->heightFactor = heightFactor_;
    hm->noDataValues = noDataValues_;

    size_t count = static_cast<size_t>(w * h);
    hm->heights.reserve(count);

    float minH = 1e30f, maxH = -1e30f;
    for (size_t i = 0; i < count; ++i) {
        size_t off = i * 3;
        float r = static_cast<float>(pixels[off]);
        float g = static_cast<float>(pixels[off + 1]);
        float b = static_cast<float>(pixels[off + 2]);
        float elev = 0.0f;
        if (encoding_ == Encoding::MapboxTerrainRgb) {
            elev = -10000.0f + (r * 65536.0f + g * 256.0f + b) * 0.1f;
        } else {
            elev = r * 256.0f + g + b / 256.0f - 32768.0f;
        }
        elev *= heightFactor_;  // OpenGlobus _heightFactor
        hm->heights.push_back(elev);
        if (elev < minH) minH = elev;
        if (elev > maxH) maxH = elev;
    }
    hm->minHeight = minH;
    hm->maxHeight = maxH;
    stbi_image_free(pixels);
    return hm;
#else
    (void)data;
    (void)len;
    return nullptr;
#endif
}

} // namespace earth_engine
