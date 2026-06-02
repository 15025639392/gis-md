#include "CurlPlatformBridge.h"

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

#include <thread>
#include <mutex>
#include <cstring>
#include <cstdio>

namespace earth_engine {

// ============================================================
// CURL 全局初始化
// ============================================================

namespace {
#if EARTH_ENGINE_HAS_LIBCURL
static std::once_flag gCurlInitOnce;
static void ensureCurl() {
    std::call_once(gCurlInitOnce, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

// CURL write callback
static size_t writeCb(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* buf = static_cast<std::vector<uint8_t>*>(userp);
    size_t total = size * nmemb;
    buf->insert(buf->end(),
                static_cast<const uint8_t*>(contents),
                static_cast<const uint8_t*>(contents) + total);
    return total;
}
#endif
} // anonymous namespace

// ============================================================
// CurlHttpRequest — HttpRequest RAII 实现
// ============================================================

class CurlHttpRequest : public HttpRequest {
public:
#if EARTH_ENGINE_HAS_LIBCURL
    explicit CurlHttpRequest(CURL* easy) : easy_(easy) {}
#else
    CurlHttpRequest() = default;
#endif
    ~CurlHttpRequest() override { cancel(); }

    void cancel() override {
#if EARTH_ENGINE_HAS_LIBCURL
        if (easy_) {
            curl_easy_cleanup(easy_);
            easy_ = nullptr;
        }
#endif
    }

private:
#if EARTH_ENGINE_HAS_LIBCURL
    CURL* easy_ = nullptr;
#endif
};

// ============================================================
// CurlPlatformBridge
// ============================================================

CurlPlatformBridge::CurlPlatformBridge() {
#if EARTH_ENGINE_HAS_LIBCURL
    ensureCurl();
#endif
}

CurlPlatformBridge::~CurlPlatformBridge() = default;

void CurlPlatformBridge::onMemoryPressure() {}
void CurlPlatformBridge::onEnterBackground() {}
void CurlPlatformBridge::onEnterForeground() {}

std::unique_ptr<HttpRequest> CurlPlatformBridge::get(
    const std::string& url,
    std::function<void(int, std::vector<uint8_t>)> callback) {

#if EARTH_ENGINE_HAS_LIBCURL
    CURL* curl = curl_easy_init();
    if (!curl) {
        callback(-1, {});
        return nullptr;
    }

    auto* body = new std::vector<uint8_t>();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "earth-md/0.1");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);

    auto request = std::make_unique<CurlHttpRequest>(curl);

    // 在后台线程执行
    std::thread([curl, body, callback = std::move(callback)]() {
        CURLcode res = curl_easy_perform(curl);

        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_easy_cleanup(curl);

        int statusCode = (res == CURLE_OK) ? static_cast<int>(httpCode) : -1;
        std::vector<uint8_t> result = (statusCode == 200)
                                          ? std::move(*body)
                                          : std::vector<uint8_t>{};
        delete body;
        callback(statusCode, std::move(result));
    }).detach();

    return request;
#else
    (void)url;
    callback(-1, {});
    return std::make_unique<CurlHttpRequest>();
#endif
}

std::string CurlPlatformBridge::cacheDirectory() const {
    return "/tmp/earth_engine_cache";
}

std::string CurlPlatformBridge::documentsDirectory() const {
    return ".";
}

std::unique_ptr<DecodedImage> CurlPlatformBridge::decodeImage(
    const uint8_t* data, size_t len) {
#if EARTH_ENGINE_HAS_STB_IMAGE
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        data, static_cast<int>(len), &width, &height, &channels, 4);

    if (!pixels) return nullptr;

    auto image = std::make_unique<DecodedImage>();
    image->width = width;
    image->height = height;
    image->channels = 4;
    image->pixels.assign(pixels,
                         pixels + static_cast<size_t>(width * height * 4));
    stbi_image_free(pixels);
    return image;
#else
    (void)data;
    (void)len;
    return nullptr;
#endif
}

void CurlPlatformBridge::log(LogLevel /*level*/, const std::string& tag,
                              const std::string& message) {
    fprintf(stderr, "[%s] %s\n", tag.c_str(), message.c_str());
}

DeviceInfo CurlPlatformBridge::deviceInfo() const {
    DeviceInfo info;
    info.platform = "cross-platform";
    info.cpuCores = 4;
    return info;
}

std::string CurlPlatformBridge::getToken(const std::string& /*providerId*/) const {
    return "";
}

} // namespace earth_engine
