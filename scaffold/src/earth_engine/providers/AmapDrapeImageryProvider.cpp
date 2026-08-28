#include "AmapDrapeImageryProvider.h"

#include <algorithm>
#include <atomic>
#include <utility>

#include "../core/async/AsyncSystem.h"
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

std::unique_ptr<DecodedImage> transparentImage(int size) {
    VectorRasterImage raster;
    raster.size = std::max(1, size);
    raster.rgba.assign(static_cast<size_t>(raster.size) * raster.size * 4, 0);
    return toDecodedImage(std::move(raster));
}

}  // namespace

struct AmapDrapeImageryProvider::Assembly {
    TileKey pageKey;
    CancellationToken token;
    TileCallback callback;
    MercatorRect rect;
    bool gcj = false;
    int styleZoom = 0;
    int tileSize = 256;
    VectorRasterStyle style;
    std::weak_ptr<ThreadPool> rasterPool;

    std::vector<std::shared_ptr<const AmapDecodedTile>> holders;
    std::atomic<int> remaining{0};
};

void AmapDrapeImageryProvider::runAssembly(
    const std::shared_ptr<Assembly>& assembly) {
    if (assembly->token.isCancelled()) {
        assembly->callback(assembly->pageKey, nullptr);
        return;
    }
    std::vector<const Feature*> features;
    std::vector<std::vector<Feature>> converted;
    for (const auto& holder : assembly->holders) {
        if (!holder) continue;
        for (const AmapDecodedLayerPart& part : holder->parts) {
            if (part.type != 2) continue;
            converted.push_back(amapDecodedPartToFeatures(part, false));
            for (const Feature& f : converted.back()) {
                if (f.type == GeometryType::Polygon) features.push_back(&f);
            }
        }
    }
    static const UnitTransform kGcjXform = mvt_rect::wgsUnitToGcjUnit;
    const UnitTransform* xf = assembly->gcj ? &kGcjXform : nullptr;
    VectorRasterImage raster = rasterizeFeaturePolygonsRect(
        features, assembly->rect, assembly->styleZoom, assembly->style,
        assembly->tileSize, xf);
    if (raster.empty()) {
        assembly->callback(assembly->pageKey,
                           transparentImage(assembly->tileSize));
        return;
    }
    assembly->callback(assembly->pageKey, toDecodedImage(std::move(raster)));
}

void AmapDrapeImageryProvider::completeIfReady(
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

AmapDrapeImageryProvider::AmapDrapeImageryProvider(
    Options options, std::shared_ptr<RegionCache> tileCache,
    std::shared_ptr<ThreadPool> rasterPool)
    : options_(std::move(options)),
      tileCache_(std::move(tileCache)),
      rasterPool_(std::move(rasterPool)) {}

void AmapDrapeImageryProvider::setStyle(VectorRasterStyle style) {
    {
        std::lock_guard<std::mutex> lock(styleMutex_);
        options_.style = std::move(style);
    }
    contentRevision_.fetch_add(1, std::memory_order_release);
}

VectorRasterStyle AmapDrapeImageryProvider::styleSnapshot() const {
    std::lock_guard<std::mutex> lock(styleMutex_);
    return options_.style;
}

std::string AmapDrapeImageryProvider::buildUrl(const TileKey& key) const {
    return options_.id + "://" + std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" + std::to_string(key.y);
}

void AmapDrapeImageryProvider::requestTile(const TileKey& key,
                                           CancellationToken token,
                                           TileCallback callback,
                                           HttpRequestPriority) {
    if (!callback) return;
    if (!tileCache_) {
        callback(key, nullptr);
        return;
    }

    const MercatorRect rectPage =
        mvt_rect::tileToUnitRect(key.z, key.x, key.y);
    // 选瓦:Amap 4326 网格是 GCJ。页是 WGS mercator 时先把矩形搬到 GCJ。
    const MercatorRect rectForTiles = options_.gcj02SourceGrid
                                          ? rectPage
                                          : mvt_rect::shiftRectWgs84ToGcj(rectPage);
    const int dataZ = std::max(options_.dataMinZoom,
                               std::min(key.z, options_.dataMaxZoom));
    std::vector<mvt_rect::TileXY> tiles =
        mvt_rect::amapCoverageFromUnitRect(rectForTiles, dataZ);
    if (static_cast<int>(tiles.size()) > options_.maxSourceTiles) {
        callback(key, transparentImage(options_.tileSize));
        return;
    }
    if (tiles.empty()) {
        callback(key, transparentImage(options_.tileSize));
        return;
    }

    auto assembly = std::make_shared<Assembly>();
    assembly->pageKey = key;
    assembly->token = token;
    assembly->callback = std::move(callback);
    assembly->rect = rectPage;
    assembly->gcj = options_.gcj02SourceGrid;
    assembly->styleZoom = key.z;
    assembly->tileSize = options_.tileSize;
    assembly->style = styleSnapshot();
    assembly->rasterPool = rasterPool_;
    assembly->holders.assign(tiles.size(), nullptr);
    assembly->remaining.store(static_cast<int>(tiles.size()),
                              std::memory_order_release);

    for (size_t i = 0; i < tiles.size(); ++i) {
        TileKey fetchKey{"Amap-Geographic", dataZ, tiles[i].x, tiles[i].y};
        tileCache_->request(
            fetchKey,
            [assembly, i](std::shared_ptr<const AmapDecodedTile> tile) {
                assembly->holders[i] = std::move(tile);
                if (assembly->remaining.fetch_sub(
                        1, std::memory_order_acq_rel) == 1) {
                    completeIfReady(assembly);
                }
            });
    }
}

std::unique_ptr<DecodedImage> AmapDrapeImageryProvider::decodeTile(
    const uint8_t* data, size_t len) {
    AmapDecodedTile tile;
    std::string error;
    if (!data || len == 0 ||
        !AmapDecodedTileDecodeTraits::decode(data, len, tile, &error)) {
        return nullptr;
    }
    std::vector<const Feature*> ptrs;
    std::vector<std::vector<Feature>> converted;
    for (const AmapDecodedLayerPart& part : tile.parts) {
        if (part.type != 2) continue;
        converted.push_back(amapDecodedPartToFeatures(part, false));
        for (const Feature& feature : converted.back()) {
            ptrs.push_back(&feature);
        }
    }
    return toDecodedImage(rasterizeFeaturePolygonsRect(
        ptrs, MercatorRect{0.0, 0.0, 1.0, 1.0}, options_.dataMaxZoom,
        styleSnapshot(), options_.tileSize));
}

}  // namespace earth_engine
