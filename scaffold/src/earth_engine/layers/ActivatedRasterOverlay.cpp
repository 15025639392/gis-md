#include "ActivatedRasterOverlay.h"
#include "RasterOverlay.h"
#include "../providers/RasterOverlayTileProvider.h"

namespace earth_engine {

ActivatedRasterOverlay::ActivatedRasterOverlay(RasterOverlay& overlay)
    : overlay_(overlay)
    , maximumSimultaneousTileLoads_(overlay.getOptions().maximumSimultaneousTileLoads) {}

ActivatedRasterOverlay::~ActivatedRasterOverlay() = default;

void ActivatedRasterOverlay::setTileProvider(
    std::unique_ptr<RasterOverlayTileProvider> provider) {
    tileProvider_ = std::move(provider);
    if (tileProvider_) {
        tileProvider_->setOwner(&overlay_);
    }
}

RasterOverlayTile* ActivatedRasterOverlay::getPlaceholderTile() {
    if (!tileProvider_) return nullptr;
    return tileProvider_->getPlaceholderTile();
}

RasterOverlayTile* ActivatedRasterOverlay::getTile(const TileKey& key) {
    if (!tileProvider_) return nullptr;
    return tileProvider_->getTile(key);
}

bool ActivatedRasterOverlay::loadTileThrottled(RasterOverlayTile& tile) {
    if (!tileProvider_) return false;

    if (tileProvider_->getThrottledTilesCurrentlyLoading() >=
        maximumSimultaneousTileLoads_) {
        return false;
    }

    return tileProvider_->loadTile(tile);
}

void ActivatedRasterOverlay::processPendingUploads() {
    if (tileProvider_) {
        tileProvider_->processPendingUploads();
    }
}

void ActivatedRasterOverlay::setFrameNumber(uint64_t frameNumber) {
    if (tileProvider_) {
        tileProvider_->setFrameNumber(frameNumber);
    }
}

void ActivatedRasterOverlay::trimUnusedTiles() {
    if (tileProvider_) {
        tileProvider_->trimUnusedTiles();
    }
}

int ActivatedRasterOverlay::getCachedTileCount() const {
    return tileProvider_ ? tileProvider_->getCachedTileCount() : 0;
}

int ActivatedRasterOverlay::getThrottledTilesCurrentlyLoading() const {
    if (!tileProvider_) return 0;
    return tileProvider_->getThrottledTilesCurrentlyLoading();
}

bool ActivatedRasterOverlay::visible() const {
    return overlay_.visible();
}

float ActivatedRasterOverlay::opacity() const {
    return overlay_.opacity();
}

} // namespace earth_engine
