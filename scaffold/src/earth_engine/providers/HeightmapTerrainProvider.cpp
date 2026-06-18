#include "HeightmapTerrainProvider.h"
#include "../core/async/AsyncSystem.h"
#include "../core/cache/HttpCache.h"
#include "../platform/bridge/CurlMultiRequestScheduler.h"
#include "../platform/bridge/PlatformBridge.h"

#if __has_include(<stb_image.h>)
#include <stb_image.h>
#define EARTH_ENGINE_HAS_STB_IMAGE 1
#else
#define EARTH_ENGINE_HAS_STB_IMAGE 0
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace earth_engine {

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
                                            HeightmapCallback callback,
                                            HttpRequestPriority priority) {
    std::string url = buildUrl(key);
    // cesium-native alignment: use thread pool instead of raw std::thread::detach().
    AsyncSystem::pool().enqueue(
        [this, url, key, token = std::move(token),
         priority,
         callback = std::move(callback)]() mutable {
            if (token.isCancelled()) {
                callback(key, TerrainTileLoadResult::cancelled());
                return;
            }
            auto body = httpGet(url, token, priority);
            if (token.isCancelled()) {
                callback(key, TerrainTileLoadResult::cancelled());
                return;
            }
            if (body.empty()) {
                callback(key, TerrainTileLoadResult::retryLater());
                return;
            }
            auto hm = decodeTile(body.data(), body.size());
            callback(key, TerrainTileLoadResult::success(std::move(hm)));
        });
}

std::vector<uint8_t> HeightmapTerrainProvider::httpGet(
    const std::string& url,
    const CancellationToken& token,
    HttpRequestPriority priority) {
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
        struct WaitState {
            std::vector<uint8_t> result;
            std::mutex mutex;
            std::condition_variable cv;
            bool done = false;
        };
        auto state = std::make_shared<WaitState>();

        auto request = platformBridge_->get(url, [state](int code, std::vector<uint8_t> body) {
            {
                std::lock_guard<std::mutex> lk(state->mutex);
                if (code == 200) state->result = std::move(body);
                state->done = true;
            }
            state->cv.notify_one();
        }, {priority});

        bool done = false;
        {
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(20);
            std::unique_lock<std::mutex> lk(state->mutex);
            while (!state->done && !token.isCancelled()) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) break;
                state->cv.wait_until(
                    lk,
                    std::min(deadline, now + std::chrono::milliseconds(20)));
            }
            done = state->done;
        }
        if ((!done || token.isCancelled()) && request) {
            request->cancel();
        }
        if (done && !state->result.empty()) HttpCache::shared().put(url, state->result);
        return done ? std::move(state->result) : std::vector<uint8_t>{};
    }

    std::vector<uint8_t> result =
        CurlMultiRequestScheduler::shared().getBlocking(
            url,
            {priority},
            std::chrono::seconds(20),
            [&token]() { return token.isCancelled(); });
    if (!result.empty()) HttpCache::shared().put(url, result);
    return result;
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
