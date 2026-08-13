#include "VectorDrapeImageryProvider.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

#include "../core/async/AsyncSystem.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Gcj02CoordinateTransform.h"
#include "../data/MvtDecoder.h"

namespace earth_engine {

namespace {

constexpr double kPi = 3.14159265358979323846;

uint64_t packDataKey(int z, int x, int y) {
    return (static_cast<uint64_t>(z) << 48) |
           (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 24) |
           static_cast<uint64_t>(static_cast<uint32_t>(y));
}

MercatorRect tileToUnitRect(int z, int x, int y) {
    const double span = 1.0 / static_cast<double>(1ull << z);
    return MercatorRect{x * span, y * span, (x + 1) * span, (y + 1) * span};
}

double unitXFromLongitude(double lngRad) { return lngRad / (2.0 * kPi) + 0.5; }

double unitYFromLatitude(double latRad) {
    return 0.5 - std::log(std::tan(kPi / 4.0 + latRad / 2.0)) / (2.0 * kPi);
}

double longitudeFromUnitX(double u) { return (u - 0.5) * 2.0 * kPi; }

double latitudeFromUnitY(double v) {
    return 2.0 * std::atan(std::exp(kPi * (1.0 - 2.0 * v))) - kPi / 2.0;
}

/// GCJ 网格矩形 → 真实 WGS84 覆盖区:按中心点 toWgs84 做常量平移。与渲染侧
/// per-tile 单点 worldOffset(TerrainPageStore determination)同语义同精度 ——
/// 两侧都是"整矩形一个偏移",偏移场梯度(~1e-5)在页跨度内的残差远小于纹素。
MercatorRect shiftRectGcjToWgs84(const MercatorRect& rect) {
    const double cu = 0.5 * (rect.x0 + rect.x1);
    const double cv = 0.5 * (rect.y0 + rect.y1);
    const Cartographic gcj = Cartographic::fromRadians(
        longitudeFromUnitX(cu), latitudeFromUnitY(cv));
    const Cartographic wgs = Gcj02CoordinateTransform::toWgs84(gcj);
    const double du = unitXFromLongitude(wgs.longitude()) - cu;
    const double dv = unitYFromLatitude(wgs.latitude()) - cv;
    return MercatorRect{rect.x0 + du, rect.y0 + dv, rect.x1 + du,
                        rect.y1 + dv};
}

std::unique_ptr<DecodedImage> toDecodedImage(VectorRasterImage&& raster) {
    if (raster.empty()) return nullptr;
    auto image = std::make_unique<DecodedImage>();
    image->width = raster.size;
    image->height = raster.size;
    image->channels = 4;
    image->bytesPerChannel = 1;
    image->pixels = std::move(raster.rgba);
    return image;
}

} // namespace

/// 一次 requestTile 的聚合状态:等覆盖矩形的全部源瓦到齐(命中/拉取/失败)。
/// slot 由 dataKey 唯一对应,不同源瓦的完成回调并发写不同 slot,互不冲突;
/// 计数原子递减,归零者负责收尾 —— 无需再持锁。
struct VectorDrapeImageryProvider::Assembly {
    TileKey pageKey;
    CancellationToken token;
    TileCallback callback;
    MercatorRect rect;
    int styleZoom = 0;
    int tileSize = 256;
    VectorRasterStyle style;  // 按值快照:任务生命周期可超 provider
    ThreadPool* rasterPool = nullptr;

    std::vector<uint64_t> dataKeys;
    std::vector<MvtTileRef> refs;  // tile 指针延迟到收尾时从 holders 取
    std::vector<std::shared_ptr<const MvtTile>> holders;
    std::atomic<int> remaining{0};
};

VectorDrapeImageryProvider::VectorDrapeImageryProvider(Options options,
                                                       FetchFn fetch,
                                                       ThreadPool* rasterPool)
    : options_(std::move(options)),
      fetch_(std::move(fetch)),
      rasterPool_(rasterPool) {}

std::string VectorDrapeImageryProvider::buildUrl(const TileKey& key) const {
    return options_.id + "://" + std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" + std::to_string(key.y);
}

void VectorDrapeImageryProvider::completeIfReady(
    const std::shared_ptr<Assembly>& assembly) {
    if (assembly->remaining.load(std::memory_order_acquire) != 0) {
        return;
    }
    auto work = [assembly]() {
        // 取消后仍要回调:上层按「回调必到」管理在途计数。取消路径页已
        // 淘汰,nullptr 会被 kickPageFetches 的回调直接丢弃。
        if (assembly->token.isCancelled()) {
            assembly->callback(assembly->pageKey, nullptr);
            return;
        }
        for (size_t i = 0; i < assembly->refs.size(); ++i) {
            assembly->refs[i].tile = assembly->holders[i].get();
        }
        VectorRasterImage raster = rasterizeMvtRect(
            assembly->refs, assembly->rect, assembly->styleZoom,
            assembly->style, assembly->tileSize);
        // 空区域/全部源瓦失败 → 全透明图,同样有效:页存储的 assembler 必须
        // 收到每个源才 complete(见类头注释的失败语义)。
        assembly->callback(assembly->pageKey,
                           toDecodedImage(std::move(raster)));
    };
    if (assembly->rasterPool) {
        assembly->rasterPool->enqueue(std::move(work));
    } else {
        work();
    }
}

void VectorDrapeImageryProvider::onSourceTileReady(
    uint64_t dataKey, std::shared_ptr<const MvtTile> tile) {
    std::vector<std::shared_ptr<Assembly>> waiters;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = inflight_.find(dataKey);
        if (it != inflight_.end()) {
            waiters = std::move(it->second);
            inflight_.erase(it);
        }
        if (tile) {
            cacheOrder_.push_front(CacheEntry{dataKey, tile});
            cache_[dataKey] = cacheOrder_.begin();
            while (cache_.size() > options_.tileCacheCapacity) {
                cache_.erase(cacheOrder_.back().key);
                cacheOrder_.pop_back();
            }
        }
        // 失败(tile==null)不入缓存:下一批页会重新尝试,server 恢复后自愈。
    }
    for (const std::shared_ptr<Assembly>& assembly : waiters) {
        for (size_t i = 0; i < assembly->dataKeys.size(); ++i) {
            if (assembly->dataKeys[i] == dataKey) {
                assembly->holders[i] = tile;
                break;
            }
        }
        if (assembly->remaining.fetch_sub(1, std::memory_order_acq_rel) ==
            1) {
            completeIfReady(assembly);
        }
    }
}

void VectorDrapeImageryProvider::requestTile(const TileKey& key,
                                             CancellationToken token,
                                             TileCallback callback,
                                             HttpRequestPriority) {
    if (!callback) {
        return;
    }
    if (!fetch_) {
        callback(key, nullptr);
        return;
    }

    MercatorRect rect = tileToUnitRect(key.z, key.x, key.y);
    if (options_.gcj02SourceGrid) {
        rect = shiftRectGcjToWgs84(rect);
    }

    const int dataZ = std::max(0, std::min(key.z, options_.dataMaxZoom));
    const int n = 1 << dataZ;
    const auto clampTile = [n](int v) { return std::max(0, std::min(v, n - 1)); };
    // 右/下边界用 ceil-1:边界恰在瓦缝(无 GCJ 平移时的常态)不多取一排。
    const int tx0 = clampTile(static_cast<int>(std::floor(rect.x0 * n)));
    const int tx1 = clampTile(
        std::max(tx0, static_cast<int>(std::ceil(rect.x1 * n)) - 1));
    const int ty0 = clampTile(static_cast<int>(std::floor(rect.y0 * n)));
    const int ty1 = clampTile(
        std::max(ty0, static_cast<int>(std::ceil(rect.y1 * n)) - 1));

    auto assembly = std::make_shared<Assembly>();
    assembly->pageKey = key;
    assembly->token = token;
    assembly->callback = std::move(callback);
    assembly->rect = rect;
    assembly->styleZoom = key.z;
    assembly->tileSize = options_.tileSize;
    assembly->style = options_.style;
    assembly->rasterPool = rasterPool_;
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            assembly->dataKeys.push_back(packDataKey(dataZ, tx, ty));
            MvtTileRef ref;
            ref.z = dataZ;
            ref.x = tx;
            ref.y = ty;
            assembly->refs.push_back(ref);
        }
    }
    assembly->holders.assign(assembly->dataKeys.size(), nullptr);

    // 命中的当场填;miss 的挂在途(已有人拉则搭车,否则自己发起)。
    std::vector<size_t> toFetch;
    int missing = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < assembly->dataKeys.size(); ++i) {
            const uint64_t dk = assembly->dataKeys[i];
            auto it = cache_.find(dk);
            if (it != cache_.end()) {
                // LRU touch:移到头部。
                cacheOrder_.splice(cacheOrder_.begin(), cacheOrder_,
                                   it->second);
                assembly->holders[i] = it->second->tile;
                ++cacheHits_;
                continue;
            }
            ++missing;
            auto inIt = inflight_.find(dk);
            if (inIt != inflight_.end()) {
                inIt->second.push_back(assembly);
            } else {
                inflight_[dk].push_back(assembly);
                toFetch.push_back(i);
                ++cacheFetches_;
            }
        }
        assembly->remaining.store(missing, std::memory_order_release);
    }

    if (missing == 0) {
        completeIfReady(assembly);
        return;
    }
    for (size_t i : toFetch) {
        const uint64_t dk = assembly->dataKeys[i];
        TileKey fetchKey = key;  // 沿用 schemeId,z/x/y 换成数据瓦
        fetchKey.z = assembly->refs[i].z;
        fetchKey.x = assembly->refs[i].x;
        fetchKey.y = assembly->refs[i].y;
        // 捕 this 安全性:provider 由 RasterOverlay 持有,生命周期到引擎
        // 拆除;拆除时页存储先 cancel 全部在途(providers_ 变更即整店清),
        // 与 MvtVectorSource 对 fetch 回调的既有假设一致。
        fetch_(fetchKey, [this, dk](int statusCode,
                                    std::vector<uint8_t> body) {
            std::shared_ptr<const MvtTile> tile;
            if (statusCode == 200 && !body.empty()) {
                auto decoded = std::make_shared<MvtTile>();
                if (decodeMvtTile(body.data(), body.size(), *decoded)) {
                    tile = std::move(decoded);
                }
            }
            onSourceTileReady(dk, std::move(tile));
        });
    }
}

std::unique_ptr<DecodedImage> VectorDrapeImageryProvider::decodeTile(
    const uint8_t* data, size_t len) {
    MvtTile tile;
    if (!data || len == 0 || !decodeMvtTile(data, len, tile)) {
        return nullptr;
    }
    return toDecodedImage(rasterizeMvtTile(tile, options_.dataMaxZoom,
                                           options_.style,
                                           options_.tileSize));
}

VectorDrapeImageryProvider::CacheStats
VectorDrapeImageryProvider::cacheStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return CacheStats{cacheHits_, cacheFetches_};
}

} // namespace earth_engine
