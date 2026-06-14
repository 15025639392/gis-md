#include "QuantizedMeshTerrainProvider.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include "../core/async/AsyncSystem.h"
#include "../core/cache/HttpCache.h"
#include "../platform/bridge/PlatformBridge.h"
#include "../terrain/QuantizedMeshParser.h"
#include <nlohmann/json.hpp>

#ifndef EARTH_ENGINE_HAS_LIBCURL
#if !defined(ANDROID) && __has_include(<curl/curl.h>)
#include <curl/curl.h>
#define EARTH_ENGINE_HAS_LIBCURL 1
#else
#define EARTH_ENGINE_HAS_LIBCURL 0
#endif
#endif

#include <sstream>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

namespace earth_engine {

// ============================================================
// libcurl helpers
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

QuantizedMeshTerrainProvider::QuantizedMeshTerrainProvider(
    std::string urlTemplate, std::string attribution)
    : urlTemplate_(std::move(urlTemplate)),
      attribution_(std::move(attribution)) {}

QuantizedMeshTerrainProvider::~QuantizedMeshTerrainProvider() = default;

void QuantizedMeshTerrainProvider::setPlatformBridge(PlatformBridge* bridge) {
    platformBridge_ = bridge;
}

void QuantizedMeshTerrainProvider::setZoomRange(int minZ, int maxZ) {
    minZoom_ = minZ;
    maxZoom_ = maxZ;
}

namespace {

std::string layerBaseUrl(const std::string& layerJsonUrl) {
    const size_t slash = layerJsonUrl.find_last_of('/');
    if (slash == std::string::npos) return "";
    return layerJsonUrl.substr(0, slash + 1);
}

std::string resolveTerrainTemplate(const std::string& layerJsonUrl,
                                   const std::string& tileTemplate) {
    std::string normalized = tileTemplate;
    while (normalized.rfind("./", 0) == 0) {
        normalized.erase(0, 2);
    }
    if (tileTemplate.rfind("http://", 0) == 0 ||
        tileTemplate.rfind("https://", 0) == 0 ||
        tileTemplate.rfind("file://", 0) == 0) {
        return tileTemplate;
    }
    return layerBaseUrl(layerJsonUrl) + normalized;
}

} // namespace

bool QuantizedMeshTerrainProvider::configureFromLayerJsonUrl(
    const std::string& layerJsonUrl) {
    auto bytes = httpGet(layerJsonUrl);
    if (bytes.empty()) return false;
    std::string body(bytes.begin(), bytes.end());
    return configureFromLayerJson(body, layerJsonUrl);
}

bool QuantizedMeshTerrainProvider::configureFromLayerJson(
    const std::string& layerJson,
    const std::string& layerJsonUrl) {
    try {
        auto j = nlohmann::json::parse(layerJson);
        if (j.value("format", "") != "quantized-mesh-1.0") return false;
        if (j.value("projection", "EPSG:4326") != "EPSG:4326") return false;
        if (j.value("scheme", "tms") != "tms") return false;

        if (j.contains("tiles") && j["tiles"].is_array() && !j["tiles"].empty() &&
            j["tiles"][0].is_string()) {
            urlTemplate_ = resolveTerrainTemplate(layerJsonUrl,
                                                  j["tiles"][0].get<std::string>());
        }
        minZoom_ = j.value("minzoom", minZoom_);
        maxZoom_ = j.value("maxzoom", maxZoom_);
        layerJsonUrl_ = layerJsonUrl;
        availabilityRanges_.clear();
        if (j.contains("available") && j["available"].is_array()) {
            availabilityRanges_.resize(j["available"].size());
            for (size_t level = 0; level < j["available"].size(); ++level) {
                const auto& levelRanges = j["available"][level];
                if (!levelRanges.is_array()) continue;
                for (const auto& range : levelRanges) {
                    if (!range.is_object()) continue;
                    availabilityRanges_[level].push_back({
                        range.value("startX", 0),
                        range.value("startY", 0),
                        range.value("endX", 0),
                        range.value("endY", 0)
                    });
                }
            }
        }
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "QMTerrain",
            "layer.json OK: url=%s z=%d-%d ranges=%zu",
            urlTemplate_.c_str(), minZoom_, maxZoom_, availabilityRanges_.size());
#endif
        return !urlTemplate_.empty();
    } catch (...) {
        return false;
    }
}

bool QuantizedMeshTerrainProvider::supportsTile(const TileKey& key) const {
    if (key.z < minZoom_ || key.z > maxZoom_) return false;
    if (key.schemeId != schemeId()) return false;
    if (availabilityRanges_.empty()) return true;

    // cesium-native QuadtreeRectangleAvailability::isTileAvailable:
    // check ancestor levels too. A child tile is available if ANY
    // ancestor level has a range that covers the child's area.
    for (int level = 0; level <= key.z; ++level) {
        if (level < 0 || static_cast<size_t>(level) >= availabilityRanges_.size()) {
            continue;
        }
        const auto& ranges = availabilityRanges_[static_cast<size_t>(level)];
        if (ranges.empty()) continue;

        // Map the query tile's coordinates to this ancestor level
        int levelDiff = key.z - level;
        int ancestorX = key.x >> levelDiff;
        int ancestorY = key.y >> levelDiff;

        for (const auto& range : ranges) {
            if (ancestorX >= range[0] && ancestorY >= range[1] &&
                ancestorX <= range[2] && ancestorY <= range[3]) {
                return true;
            }
        }
    }
    return false;
}

std::string QuantizedMeshTerrainProvider::id() const {
    std::ostringstream oss;
    oss << "qmesh-" << std::hash<std::string>{}(urlTemplate_);
    return oss.str();
}

std::string QuantizedMeshTerrainProvider::buildUrl(const TileKey& key) const {
    std::string url = urlTemplate_;
    const int tilesAtZoom = 1 << key.z;
    const int urlY = flipYForUrl_
        ? std::clamp(tilesAtZoom - 1 - key.y, 0, tilesAtZoom - 1)
        : key.y;
    auto replace = [&url](const std::string& ph, const std::string& val) {
        size_t pos = 0;
        while ((pos = url.find(ph, pos)) != std::string::npos) {
            url.replace(pos, ph.length(), val);
            pos += val.length();
        }
    };
    replace("{z}", std::to_string(key.z));
    replace("{x}", std::to_string(key.x));
    replace("{y}", std::to_string(urlY));
    return url;
}

void QuantizedMeshTerrainProvider::requestTile(const TileKey& key,
                                                CancellationToken token,
                                                HeightmapCallback callback) {
    std::string url = buildUrl(key);
    AsyncSystem::pool().enqueue(
        [this, url, key, token = std::move(token),
         callback = std::move(callback)]() mutable {
            if (token.isCancelled()) { callback(key, nullptr); return; }
            auto body = httpGet(url);
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_INFO, "QMTerrain",
                "requestTile: z=%d x=%d y=%d body=%zu canceled=%d",
                key.z, key.x, key.y, body.size(), token.isCancelled());
#endif
            if (token.isCancelled() || body.empty()) { callback(key, nullptr); return; }
            auto hm = decodeTile(body.data(), body.size());
            callback(key, std::move(hm));
        });
}

void QuantizedMeshTerrainProvider::addAvailabilityRects(
    int level, const std::vector<std::array<int, 4>>& rects) {
    if (level < 0) return;
    if (static_cast<size_t>(level) >= availabilityRanges_.size()) {
        availabilityRanges_.resize(static_cast<size_t>(level) + 1);
    }
    auto& ranges = availabilityRanges_[static_cast<size_t>(level)];
    for (const auto& r : rects) {
        ranges.push_back(r);
    }
}

std::unique_ptr<DecodedHeightmap> QuantizedMeshTerrainProvider::decodeTile(
    const uint8_t* data, size_t len) {
    // Rasterize to regular heightmap grid for sampleHeight queries
    auto hm = QuantizedMeshParser::parseAndRasterize(data, len, tileSize_ - 1);
    if (!hm) return nullptr;

    // Preserve raw binary for on-demand triangulated mesh reconstruction
    hm->rawData.assign(data, data + len);
    return hm;
}

std::vector<uint8_t> QuantizedMeshTerrainProvider::httpGet(const std::string& url) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "QMTerrain", "httpGet ENTER: %s", url.c_str());
#endif
    // Check shared LRU cache
    auto cached = HttpCache::shared().get(url);
#ifdef __ANDROID__
    if (!cached.empty())
        __android_log_print(ANDROID_LOG_INFO, "QMTerrain",
            "httpGet cache hit: %zu bytes", cached.size());
#endif
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
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "QMTerrain",
            "httpGet bridge: %zu bytes", result.size());
#endif
        return result;
    }

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

} // namespace earth_engine
