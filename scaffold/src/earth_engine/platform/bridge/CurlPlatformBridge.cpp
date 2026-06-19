#include "CurlPlatformBridge.h"

#include "CurlMultiRequestScheduler.h"

#if __has_include(<stb_image.h>)
#include <stb_image.h>
#define EARTH_ENGINE_HAS_STB_IMAGE 1
#else
#define EARTH_ENGINE_HAS_STB_IMAGE 0
#endif

#include <cstdio>

namespace earth_engine {

CurlPlatformBridge::CurlPlatformBridge() = default;
CurlPlatformBridge::~CurlPlatformBridge() = default;

void CurlPlatformBridge::onMemoryPressure() {}

void CurlPlatformBridge::onEnterBackground() {
    CurlMultiRequestScheduler::shared().cancelQueuedRequests();
}

void CurlPlatformBridge::onEnterForeground() {}

std::unique_ptr<HttpRequest> CurlPlatformBridge::get(
    const std::string& url,
    std::function<void(int, std::vector<uint8_t>)> callback,
    HttpRequestOptions options) {
    return CurlMultiRequestScheduler::shared().get(
        url,
        std::move(callback),
        options);
}

int CurlPlatformBridge::maximumActiveRequests() const {
    return CurlMultiRequestScheduler::shared().maximumActiveRequests();
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
