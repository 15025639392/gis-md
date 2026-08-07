#include "HeightmapTerrainProvider.h"
#include "ImageTileBodyCheck.h"
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
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>

namespace earth_engine {

namespace {

// 计数必须发生在 callback 之前:TileLoadLifecycle::markDestroyingCancelAndWait
// 在 callback 内部触发 completeContentRequest 的瞬间即放行 provider 析构,
// callback 之后再写 requestsCompleted_(旧 guard 析构式计数)会踩已释放的
// atomic。completed 语义随之微变为「进入完成处理」,纯诊断字段可接受。
void noteRequestCompleted(std::atomic<int>& completed) {
    completed.fetch_add(1, std::memory_order_relaxed);
}

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
    minZoom_ = std::max(0, minZ);
    maxZoom_ = std::max(0, maxZ);
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
                                            TerrainCallback callback,
                                            HttpRequestPriority priority) {
    std::string url = buildUrl(key);
    requestsStarted_.fetch_add(1, std::memory_order_relaxed);
    if (platformBridge_) {
        if (token.isCancelled()) {
            requestsCompleted_.fetch_add(1, std::memory_order_relaxed);
            callback(key, TerrainTileLoadResult::cancelled());
            return;
        }

        auto cached = HttpCache::shared().get(url);
        if (!cached.empty() && !looksLikeImageTileBody(cached)) {
            // 历史坏体自愈(魔数校验上线前入缓存的 CDN XML 错误体):清掉
            // 后按缓存未命中走网络重取。
            HttpCache::shared().remove(url);
            cached.clear();
        }
        if (!cached.empty()) {
            // cache 命中也必须下沉 worker(与下方无 bridge 分支同模式):此处
            // 曾在主线程同步 decode+callback(callback 内含整块网格构建),
            // 运动重访已缓存区域时批量命中(每个 2-4ms)= requestMissingContent
            // 的 request 分发尖刺(帧 14-24ms)真因,真机逐时间戳对齐坐实。
            AsyncSystem::pool().enqueue(
                [this,
                 body = std::move(cached),
                 key,
                 token = std::move(token),
                 callback = std::move(callback)]() mutable {
                    noteRequestCompleted(requestsCompleted_);
                    if (token.isCancelled()) {
                        callback(key, TerrainTileLoadResult::cancelled());
                        return;
                    }
                    auto hm = decodeTile(body.data(), body.size());
                    callback(
                        key,
                        TerrainTileLoadResult::successWithHeightmap(
                            std::move(hm)));
                });
            return;
        }

        auto requestHandle =
            std::make_shared<std::unique_ptr<HttpRequest>>();
        // 桥接真取消(见 HttpRequestOptions.cancelFlag)。
        HttpRequestOptions httpOptions{priority};
        httpOptions.cancelFlag = token.sharedFlag();
        *requestHandle = platformBridge_->get(
            url,
            [this,
             url,
             key,
             token = std::move(token),
             callback = std::move(callback),
             requestHandle](int statusCode, std::vector<uint8_t> body) mutable {
                noteRequestCompleted(requestsCompleted_);
                (void)requestHandle;
                if (token.isCancelled()) {
                    callback(key, TerrainTileLoadResult::cancelled());
                    return;
                }
                // 200 + 非图像体(CDN 边缘负缓存的 NoSuchKey XML)与非 200
                // 同路:不入缓存、RetryLater(瞬时故障,非永久 Failed)。
                if (statusCode != 200 || !looksLikeImageTileBody(body)) {
                    callback(key, TerrainTileLoadResult::retryLater());
                    return;
                }
                HttpCache::shared().put(url, body);
                auto hm = decodeTile(body.data(), body.size());
                callback(key, TerrainTileLoadResult::successWithHeightmap(std::move(hm)));
            },
            httpOptions);
        return;
    }

    auto cached = HttpCache::shared().get(url);
    if (!cached.empty() && !looksLikeImageTileBody(cached)) {
        // 同 bridge 分支:历史坏体自愈,清掉后走网络重取。
        HttpCache::shared().remove(url);
        cached.clear();
    }
    if (!cached.empty()) {
        AsyncSystem::pool().enqueue(
            [this,
             body = std::move(cached),
             key,
             token = std::move(token),
             callback = std::move(callback)]() mutable {
                noteRequestCompleted(requestsCompleted_);
                if (token.isCancelled()) {
                    callback(key, TerrainTileLoadResult::cancelled());
                    return;
                }
                auto hm = decodeTile(body.data(), body.size());
                callback(key, TerrainTileLoadResult::successWithHeightmap(std::move(hm)));
            });
        return;
    }

    constexpr const char* kFilePrefix = "file://";
    if (url.rfind(kFilePrefix, 0) == 0) {
        AsyncSystem::pool().enqueue(
            [this,
             url,
             key,
             token = std::move(token),
             priority,
             callback = std::move(callback)]() mutable {
                noteRequestCompleted(requestsCompleted_);
                if (token.isCancelled()) {
                    callback(key, TerrainTileLoadResult::cancelled());
                    return;
                }
                std::vector<uint8_t> body = httpGet(url, token, priority);
                if (token.isCancelled()) {
                    callback(key, TerrainTileLoadResult::cancelled());
                    return;
                }
                if (body.empty()) {
                    callback(key, TerrainTileLoadResult::retryLater());
                    return;
                }
                auto hm = decodeTile(body.data(), body.size());
                callback(key, TerrainTileLoadResult::successWithHeightmap(std::move(hm)));
            });
        return;
    }

    auto requestHandle =
        std::make_shared<std::unique_ptr<HttpRequest>>();
    // 桥接真取消(见 HttpRequestOptions.cancelFlag)。
    HttpRequestOptions httpOptions{priority};
    httpOptions.cancelFlag = token.sharedFlag();
    *requestHandle = CurlMultiRequestScheduler::shared().get(
        url,
        [this,
         url,
         key,
         token = std::move(token),
         callback = std::move(callback),
         requestHandle](int statusCode, std::vector<uint8_t> body) mutable {
            (void)requestHandle;
            auto tokenPtr =
                std::make_shared<CancellationToken>(std::move(token));
            auto callbackPtr =
                std::make_shared<TerrainCallback>(std::move(callback));
            auto bodyPtr =
                std::make_shared<std::vector<uint8_t>>(std::move(body));
            AsyncSystem::pool().enqueue(
                [this,
                 url,
                 key,
                 tokenPtr,
                 callbackPtr,
                 statusCode,
                 bodyPtr]() mutable {
                    noteRequestCompleted(requestsCompleted_);
                    if (tokenPtr->isCancelled()) {
                        (*callbackPtr)(
                            key,
                            TerrainTileLoadResult::cancelled());
                        return;
                    }
                    // 同 bridge 分支:200 + 非图像体不入缓存,RetryLater。
                    if (statusCode != 200 ||
                        !looksLikeImageTileBody(*bodyPtr)) {
                        (*callbackPtr)(
                            key,
                            TerrainTileLoadResult::retryLater());
                        return;
                    }
                    HttpCache::shared().put(url, *bodyPtr);
                    auto hm = decodeTile(bodyPtr->data(), bodyPtr->size());
                    (*callbackPtr)(
                        key,
                        TerrainTileLoadResult::successWithHeightmap(std::move(hm)));
                });
        },
        httpOptions);
}

ProviderRequestDiagnostics HeightmapTerrainProvider::requestDiagnostics() const {
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

    (void)token;
    (void)priority;
    return {};
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
        hm->borderInset = borderInset_;
        hm->noDataValues = noDataValues_;
        if (hasImplicitNoDataSentinel(encoding_)) {
            // Terrain-RGB 的 RGB(0,0,0) 解码恰为 -10000m = 数据源 nodata 底值
            // (缺邻居的重叠环/数据空洞都编成它)。不注册则 -10000 被当合法高度
            // 混进边缘双线性,造出 km 级假深沟(顶点被紧 near/far 裁掉 → 黑裂缝)
            // 与假悬崖法线(瓦片边界光照条带)。
            hm->noDataValues.push_back(kTerrainRgbNoDataFloorMeters * heightFactor_);
        }

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
            // min/max 只统计有效高度:nodata 混入会把量化区间拉到 -10000,
            // 16bit 高度精度被吃掉 2/3。
            if (!hm->isNoData(h)) {
                if (h < minH) minH = h;
                if (h > maxH) maxH = h;
            }
        }
        hm->minHeight = minH <= maxH ? minH : 0.0f;
        hm->maxHeight = minH <= maxH ? maxH : 0.0f;
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
    hm->borderInset = borderInset_;
    hm->noDataValues = noDataValues_;
    if (hasImplicitNoDataSentinel(encoding_)) {
        // 与 platformBridge 分支同义:注册 Terrain-RGB nodata 底值,见上方注释。
        hm->noDataValues.push_back(kTerrainRgbNoDataFloorMeters * heightFactor_);
    }

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
        if (!hm->isNoData(elev)) {
            if (elev < minH) minH = elev;
            if (elev > maxH) maxH = elev;
        }
    }
    hm->minHeight = minH <= maxH ? minH : 0.0f;
    hm->maxHeight = minH <= maxH ? maxH : 0.0f;
    stbi_image_free(pixels);
    return hm;
#else
    (void)data;
    (void)len;
    return nullptr;
#endif
}

} // namespace earth_engine
