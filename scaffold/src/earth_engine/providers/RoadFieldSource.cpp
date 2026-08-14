#include "RoadFieldSource.h"

#include <atomic>
#include <utility>

#include "../core/async/AsyncSystem.h"
#include "../data/LineFieldRasterizer.h"
#include "MvtRectCoverage.h"

namespace earth_engine {

/// 一次 requestField 的聚合态:等覆盖矩形的全部源瓦到齐。
/// slot 独占写、原子计数归零者收尾 —— 与刀1 Assembly 同构。
struct RoadFieldSource::Assembly {
    TileKey pageKey;
    CancellationToken token;
    FieldCallback callback;
    MercatorRect rect;  // 栅格化目标 = GCJ 页矩形(逐点变换承载偏移)
    bool gcj = false;   // 目标是 GCJ 采样空间 → 逐顶点 wgsUnitToGcjUnit
    int styleZoom = 0;
    int fieldSize = 256;
    VectorRasterStyle style;  // 按值快照
    std::weak_ptr<ThreadPool> rasterPool;

    std::vector<MvtTileRef> refs;
    std::vector<std::shared_ptr<const MvtTile>> holders;
    std::atomic<int> remaining{0};
};

RoadFieldSource::RoadFieldSource(Options options,
                                 std::shared_ptr<MvtTileFetchCache> tileCache,
                                 std::shared_ptr<ThreadPool> rasterPool)
    : options_(std::move(options)),
      tileCache_(std::move(tileCache)),
      rasterPool_(std::move(rasterPool)) {}

void RoadFieldSource::runAssembly(
    const std::shared_ptr<Assembly>& assembly) {
    // 取消仍回调(空场):页存储按「回调必到」记账,账本校验丢迟到结果。
    if (assembly->token.isCancelled()) {
        assembly->callback(std::vector<uint8_t>(
            static_cast<size_t>(assembly->fieldSize) * assembly->fieldSize *
                4u,
            0));
        return;
    }
    for (size_t i = 0; i < assembly->refs.size(); ++i) {
        assembly->refs[i].tile = assembly->holders[i].get();
    }
    // GCJ 目标:逐顶点 wgsUnitToGcjUnit 把 OSM 顶点搬到 GCJ 采样空间,与地形
    // 逐顶点 GCJ texcoord + 高德逐像素 GCJ 对齐。标准 overlay = nullptr 线性。
    static const UnitTransform kGcjXform = mvt_rect::wgsUnitToGcjUnit;
    const UnitTransform* xf = assembly->gcj ? &kGcjXform : nullptr;
    LineFieldImage field = rasterizeLineFieldRect(
        assembly->refs, assembly->rect, assembly->styleZoom, assembly->style,
        assembly->fieldSize, xf);
    if (field.empty()) {
        field.size = assembly->fieldSize;
        field.rgba8.assign(
            static_cast<size_t>(assembly->fieldSize) * assembly->fieldSize *
                4u,
            0);
    }
    assembly->callback(std::move(field.rgba8));
}

void RoadFieldSource::completeIfReady(
    const std::shared_ptr<Assembly>& assembly) {
    if (assembly->remaining.load(std::memory_order_acquire) != 0) {
        return;
    }
    if (std::shared_ptr<ThreadPool> pool = assembly->rasterPool.lock()) {
        pool->enqueue([assembly]() { runAssembly(assembly); });
    } else {
        runAssembly(assembly);
    }
}

void RoadFieldSource::requestField(const TileKey& pageKey,
                                   CancellationToken token,
                                   FieldCallback callback) {
    if (!callback) return;
    const int size = options_.fieldSize;
    if (!tileCache_) {
        callback(std::vector<uint8_t>(
            static_cast<size_t>(size) * size * 4u, 0));
        return;
    }

    // rectGcj = 页在 GCJ 采样空间的矩形(栅格化目标,逐点变换承载偏移)。
    // rectWgs = 真实 WGS84 覆盖区,**仅用于选取 OSM 源瓦片**。
    const MercatorRect rectGcj =
        mvt_rect::tileToUnitRect(pageKey.z, pageKey.x, pageKey.y);
    const MercatorRect rectWgs = options_.gcj02SourceGrid
                                     ? mvt_rect::shiftRectGcjToWgs84(rectGcj)
                                     : rectGcj;
    const int dataZ =
        std::max(0, std::min(pageKey.z, options_.dataMaxZoom));
    const std::vector<mvt_rect::TileXY> tiles =
        mvt_rect::coverage(rectWgs, dataZ);

    auto assembly = std::make_shared<Assembly>();
    assembly->pageKey = pageKey;
    assembly->token = token;
    assembly->callback = std::move(callback);
    assembly->rect = rectGcj;
    assembly->gcj = options_.gcj02SourceGrid;
    assembly->styleZoom = pageKey.z;
    assembly->fieldSize = size;
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
        TileKey fetchKey = pageKey;  // 沿用 schemeId
        fetchKey.z = assembly->refs[i].z;
        fetchKey.x = assembly->refs[i].x;
        fetchKey.y = assembly->refs[i].y;
        tileCache_->request(
            fetchKey,
            [assembly, i](std::shared_ptr<const MvtTile> tile) {
                assembly->holders[i] = std::move(tile);  // 失败=null,照烘
                if (assembly->remaining.fetch_sub(
                        1, std::memory_order_acq_rel) == 1) {
                    completeIfReady(assembly);
                }
            });
    }
}

} // namespace earth_engine
