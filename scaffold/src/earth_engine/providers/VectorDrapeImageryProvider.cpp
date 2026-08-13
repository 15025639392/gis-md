#include "VectorDrapeImageryProvider.h"

#include <algorithm>
#include <atomic>
#include <utility>

#include "../core/async/AsyncSystem.h"
#include "../data/MvtDecoder.h"
#include "MvtRectCoverage.h"

namespace earth_engine {

namespace {

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
/// slot 独占写(每 slot 一个回调闭包),计数原子递减,归零者负责收尾。
struct VectorDrapeImageryProvider::Assembly {
    TileKey pageKey;
    CancellationToken token;
    TileCallback callback;
    MercatorRect rect;   // 栅格化目标 = GCJ 页矩形(逐点变换承载偏移)
    bool gcj = false;
    int styleZoom = 0;
    int tileSize = 256;
    VectorRasterStyle style;  // 按值快照:任务生命周期可超 provider
    // weak:GL 会话拆除会 stop/析构线程池,而 fetch 回调(网络线程)可迟到
    // 于拆除 —— 锁不上就地栅格化(仍回调,账本校验丢迟到结果),绝不
    // enqueue 裸指针(真机崩过 `enqueue on stopped ThreadPool`)。
    std::weak_ptr<ThreadPool> rasterPool;

    std::vector<MvtTileRef> refs;  // tile 指针延迟到收尾时从 holders 取
    std::vector<std::shared_ptr<const MvtTile>> holders;
    std::atomic<int> remaining{0};
};

/// 收尾本体(可能在 rasterPool、拉取回调线程或调用线程执行)。
void VectorDrapeImageryProvider::runAssembly(
    const std::shared_ptr<Assembly>& assembly) {
    // 取消后仍要回调:上层按「回调必到」管理在途计数。取消路径页已
    // 淘汰,nullptr 会被 kickPageFetches 的回调直接丢弃。
    if (assembly->token.isCancelled()) {
        assembly->callback(assembly->pageKey, nullptr);
        return;
    }
    for (size_t i = 0; i < assembly->refs.size(); ++i) {
        assembly->refs[i].tile = assembly->holders[i].get();
    }
    // GCJ 目标:逐顶点 wgsUnitToGcjUnit 与地形 GCJ texcoord + 高德影像对齐
    // (整页平移在大页/祖先页边缘发散,已弃)。标准 overlay = nullptr 线性。
    static const UnitTransform kGcjXform = mvt_rect::wgsUnitToGcjUnit;
    const UnitTransform* xf = assembly->gcj ? &kGcjXform : nullptr;
    VectorRasterImage raster = rasterizeMvtRect(
        assembly->refs, assembly->rect, assembly->styleZoom, assembly->style,
        assembly->tileSize, xf);
    // 空区域/全部源瓦失败 → 全透明图,同样有效:页存储的 assembler 必须
    // 收到每个源才 complete(见类头注释的失败语义)。
    assembly->callback(assembly->pageKey, toDecodedImage(std::move(raster)));
}

void VectorDrapeImageryProvider::completeIfReady(
    const std::shared_ptr<Assembly>& assembly) {
    if (assembly->remaining.load(std::memory_order_acquire) != 0) {
        return;
    }
    if (std::shared_ptr<ThreadPool> pool = assembly->rasterPool.lock()) {
        pool->enqueue([assembly]() { runAssembly(assembly); });
    } else {
        // 池未注入(host 测试)或已随会话拆除:就地跑,回调仍必到。
        runAssembly(assembly);
    }
}

VectorDrapeImageryProvider::VectorDrapeImageryProvider(
    Options options, std::shared_ptr<MvtTileFetchCache> tileCache,
    std::shared_ptr<ThreadPool> rasterPool)
    : options_(std::move(options)),
      tileCache_(std::move(tileCache)),
      rasterPool_(std::move(rasterPool)) {}

std::string VectorDrapeImageryProvider::buildUrl(const TileKey& key) const {
    return options_.id + "://" + std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" + std::to_string(key.y);
}

void VectorDrapeImageryProvider::requestTile(const TileKey& key,
                                             CancellationToken token,
                                             TileCallback callback,
                                             HttpRequestPriority) {
    if (!callback) {
        return;
    }
    if (!tileCache_) {
        callback(key, nullptr);
        return;
    }

    // rectGcj = 栅格化目标(GCJ 采样空间);rectWgs = 选 OSM 源瓦用的真实范围。
    const MercatorRect rectGcj = mvt_rect::tileToUnitRect(key.z, key.x, key.y);
    const MercatorRect rectWgs = options_.gcj02SourceGrid
                                     ? mvt_rect::shiftRectGcjToWgs84(rectGcj)
                                     : rectGcj;
    const int dataZ = std::max(0, std::min(key.z, options_.dataMaxZoom));
    const std::vector<mvt_rect::TileXY> tiles =
        mvt_rect::coverage(rectWgs, dataZ);

    auto assembly = std::make_shared<Assembly>();
    assembly->pageKey = key;
    assembly->token = token;
    assembly->callback = std::move(callback);
    assembly->rect = rectGcj;
    assembly->gcj = options_.gcj02SourceGrid;
    assembly->styleZoom = key.z;
    assembly->tileSize = options_.tileSize;
    assembly->style = options_.style;
    assembly->rasterPool = rasterPool_;
    assembly->refs.reserve(tiles.size());
    for (const mvt_rect::TileXY& t : tiles) {
        MvtTileRef ref;
        ref.z = dataZ;
        ref.x = t.x;
        ref.y = t.y;
        assembly->refs.push_back(ref);
    }
    assembly->holders.assign(assembly->refs.size(), nullptr);
    assembly->remaining.store(static_cast<int>(assembly->refs.size()),
                              std::memory_order_release);

    for (size_t i = 0; i < assembly->refs.size(); ++i) {
        TileKey fetchKey = key;  // 沿用 schemeId,z/x/y 换成数据瓦
        fetchKey.z = assembly->refs[i].z;
        fetchKey.x = assembly->refs[i].x;
        fetchKey.y = assembly->refs[i].y;
        tileCache_->request(
            fetchKey,
            [assembly, i](std::shared_ptr<const MvtTile> tile) {
                assembly->holders[i] = std::move(tile);  // 失败=null,照画
                if (assembly->remaining.fetch_sub(
                        1, std::memory_order_acq_rel) == 1) {
                    completeIfReady(assembly);
                }
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

} // namespace earth_engine
