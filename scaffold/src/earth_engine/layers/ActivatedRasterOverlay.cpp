#include "ActivatedRasterOverlay.h"
#include "RasterOverlay.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "../renderer/RenderDeviceRasterTextureUploader.h"

namespace earth_engine {

ActivatedRasterOverlay::ActivatedRasterOverlay(RasterOverlay& overlay)
    : overlay_(overlay)
    , placeholderProvider_(std::make_unique<RasterOverlayTileProvider>(
          overlay.getProvider(),
          overlay.getTileScheme(),
          nullptr))
    , maximumSimultaneousTileLoads_(overlay.getOptions().maximumSimultaneousTileLoads) {
    placeholderProvider_->setOwner(&overlay_);
    placeholderProvider_->maximumSimultaneousTileLoads =
        maximumSimultaneousTileLoads_;
    placeholderProvider_->setMaximumScreenSpaceError(
        overlay_.getOptions().maximumScreenSpaceError);
}

ActivatedRasterOverlay::~ActivatedRasterOverlay() = default;

RasterOverlayTileProvider* ActivatedRasterOverlay::ensureTileProvider(
    RenderDevice* device) {
    if (!tileProvider_) {
        std::unique_ptr<RasterTextureUploader> textureUploader;
        if (device) {
            textureUploader =
                std::make_unique<RenderDeviceRasterTextureUploader>(device);
        }
        tileProvider_ = std::make_unique<RasterOverlayTileProvider>(
            overlay_.getProvider(),
            overlay_.getTileScheme(),
            std::move(textureUploader));
        tileProvider_->setOwner(&overlay_);
        tileProvider_->maximumSimultaneousTileLoads =
            maximumSimultaneousTileLoads_;
        tileProvider_->setMaximumScreenSpaceError(
            overlay_.getOptions().maximumScreenSpaceError);
    }
    return tileProvider_.get();
}

RasterOverlayTile* ActivatedRasterOverlay::getPlaceholderTile() {
    RasterOverlayTileProvider* provider =
        placeholderProvider_ ? placeholderProvider_.get() : tileProvider_.get();
    if (!provider) return nullptr;
    return provider->getPlaceholderTile().get();
}

int ActivatedRasterOverlay::processPendingUploads(
    bool interactionActive,
    FrameResourceBudget* budget) {
    if (tileProvider_) {
        return tileProvider_->processPendingUploads(interactionActive, budget);
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
    if (placeholderProvider_) {
        placeholderProvider_->maximumSimultaneousTileLoads =
            maximumSimultaneousTileLoads_;
    }
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
