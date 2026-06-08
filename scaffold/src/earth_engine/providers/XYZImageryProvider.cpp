#include "XYZImageryProvider.h"
#include "../platform/bridge/PlatformBridge.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

#if !defined(ANDROID) && __has_include(<curl/curl.h>)
#include <curl/curl.h>
#define EARTH_ENGINE_HAS_LIBCURL 1
#else
#define EARTH_ENGINE_HAS_LIBCURL 0
#endif
#if __has_include(<stb_image.h>)
#include <stb_image.h>
#define EARTH_ENGINE_HAS_STB_IMAGE 1
#else
#define EARTH_ENGINE_HAS_STB_IMAGE 0
#endif

#include <cstring>
#include <sstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace earth_engine {

// ============================================================
// libcurl 辅助（仅当无 PlatformBridge 时使用）
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
                                      TileCallback callback) {
    std::string url = buildUrl(key);
    std::thread([this, url, key, token = std::move(token),
                 callback = std::move(callback)]() {
        if (token.isCancelled()) { callback(key, nullptr); return; }
        auto body = httpGet(url, nullptr);
        if (token.isCancelled() || body.empty()) { callback(key, nullptr); return; }
        auto image = decodeTile(body.data(), body.size());
        callback(key, std::move(image));
    }).detach();
}

std::vector<uint8_t> XYZImageryProvider::httpGet(
    const std::string& url, const std::atomic<bool>* /*cancelled*/) {

    // 优先使用 PlatformBridge（Android JNI HTTP / Curl async）
    if (platformBridge_) {
        std::vector<uint8_t> result;
        std::mutex mtx;
        std::condition_variable cv;
        bool done = false;

        platformBridge_->get(url, [&](int code, std::vector<uint8_t> body) {
            if (code == 200) result = std::move(body);
            {
                std::lock_guard<std::mutex> lk(mtx);
                done = true;
            }
            cv.notify_one();
        });

        // 等待回调完成（最多 20 秒）
        {
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait_for(lk, std::chrono::seconds(20), [&]{ return done; });
        }
        return result;
    }

    // 回退：libcurl
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

    return (res == CURLE_OK && httpCode == 200) ? body : std::vector<uint8_t>{};
#else
    (void)url;
    return {};
#endif
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
