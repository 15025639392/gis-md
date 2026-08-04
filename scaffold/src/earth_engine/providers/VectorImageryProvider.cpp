#include "VectorImageryProvider.h"

#include <utility>

#include "../core/async/AsyncSystem.h"
#include "../data/MvtDecoder.h"

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

VectorImageryProvider::VectorImageryProvider(Options options, FetchFn fetch,
                                             ThreadPool* rasterPool)
    : options_(std::move(options)),
      fetch_(std::move(fetch)),
      rasterPool_(rasterPool) {}

std::string VectorImageryProvider::buildUrl(const TileKey& key) const {
    return options_.id + "://" + std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" + std::to_string(key.y);
}

void VectorImageryProvider::requestTile(const TileKey& key,
                                        CancellationToken token,
                                        TileCallback callback,
                                        HttpRequestPriority) {
    if (!fetch_ || !callback) {
        if (callback) callback(key, nullptr);
        return;
    }
    // 样式与尺寸按值捕获:worker 任务的生命周期可能超过本 provider,不能捕
    // this(RasterOverlayTileProvider 可能在瓦片在途时被替换)。
    VectorRasterStyle style = options_.style;
    const int size = options_.tileSize;
    ThreadPool* pool = rasterPool_;

    fetch_(key, [key, token, callback, style, size, pool](
                    int statusCode, std::vector<uint8_t> body) {
        auto work = [key, token, callback, style, size,
                     body = std::move(body), statusCode]() {
            // 取消后仍要回调:上层按「回调必到」管理在途计数,静默丢弃会
            // 让瓦片永远停在 pending。
            if (token.isCancelled()) {
                callback(key, nullptr);
                return;
            }
            MvtTile tile;
            if (statusCode != 200 || body.empty() ||
                !decodeMvtTile(body.data(), body.size(), tile)) {
                callback(key, nullptr);
                return;
            }
            // 空瓦片(该区域无要素)照样产出一张全透明图,而不是 nullptr ——
            // nullptr 会被上层当作「加载失败」而重试/回退祖先,但「这里确实
            // 没有路」是有效结果,回退祖先反而会画出上一档的粗路网。
            callback(key, toDecodedImage(
                              rasterizeMvtTile(tile, key.z, style, size)));
        };
        if (pool) {
            pool->enqueue(std::move(work));
        } else {
            work();
        }
    });
}

std::unique_ptr<DecodedImage> VectorImageryProvider::decodeTile(
    const uint8_t* data, size_t len) {
    return decodeTileAtZoom(data, len, options_.maxZoom);
}

std::unique_ptr<DecodedImage> VectorImageryProvider::decodeTileAtZoom(
    const uint8_t* data, size_t len, int zoom) const {
    MvtTile tile;
    if (!data || len == 0 || !decodeMvtTile(data, len, tile)) {
        return nullptr;
    }
    return toDecodedImage(
        rasterizeMvtTile(tile, zoom, options_.style, options_.tileSize));
}

} // namespace earth_engine
