#include "ActivatedRasterOverlay.h"
#include "RasterOverlay.h"
#include "../providers/RasterOverlayTileProvider.h"

namespace earth_engine {

ActivatedRasterOverlay::ActivatedRasterOverlay(RasterOverlay& overlay)
    : overlay_(overlay)
    , maximumSimultaneousTileLoads_(overlay.getOptions().maximumSimultaneousTileLoads) {}

ActivatedRasterOverlay::~ActivatedRasterOverlay() = default;

RasterOverlayTileProvider* ActivatedRasterOverlay::ensureTileProvider(
    RenderDevice* device) {
    if (!tileProvider_) {
        tileProvider_ = std::make_unique<RasterOverlayTileProvider>(
            overlay_.getProvider(),
            overlay_.getTileScheme(),
            device);
        tileProvider_->setOwner(&overlay_);
        tileProvider_->maximumSimultaneousTileLoads =
            maximumSimultaneousTileLoads_;
        tileProvider_->setMaximumScreenSpaceError(
            overlay_.getOptions().maximumScreenSpaceError);
    }
    return tileProvider_.get();
}

RasterOverlayTile* ActivatedRasterOverlay::getPlaceholderTile() {
    if (!tileProvider_) return nullptr;
    return tileProvider_->getPlaceholderTile().get();
}

int ActivatedRasterOverlay::processPendingUploads(bool interactionActive) {
    if (tileProvider_) {
        return tileProvider_->processPendingUploads(interactionActive);
    }
    return 0;
}

bool ActivatedRasterOverlay::hasPendingWork() const {
    return tileProvider_ && tileProvider_->hasPendingWork();
}

uint64_t ActivatedRasterOverlay::revision() const {
    return tileProvider_ ? tileProvider_->revision() : 0;
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

void ActivatedRasterOverlay::setMaximumSimultaneousTileLoads(int n) {
    maximumSimultaneousTileLoads_ = n > 0 ? n : 20;
    if (tileProvider_) {
        tileProvider_->maximumSimultaneousTileLoads =
            maximumSimultaneousTileLoads_;
    }
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
