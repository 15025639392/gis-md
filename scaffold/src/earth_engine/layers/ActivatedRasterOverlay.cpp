#include "ActivatedRasterOverlay.h"
#include "RasterOverlay.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "../renderer/RenderDeviceRasterTextureUploader.h"
#include "../tiling/TileRasterOverlayUploadResult.h"

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
    syncProviderOptionsFromOverlay();
    return tileProvider_.get();
}

RasterOverlayTile* ActivatedRasterOverlay::getPlaceholderTile() {
    syncProviderOptionsFromOverlay();
    RasterOverlayTileProvider* provider =
        placeholderProvider_ ? placeholderProvider_.get() : tileProvider_.get();
    if (!provider) return nullptr;
    return provider->getPlaceholderTile().get();
}

TileRasterOverlayUploadResult ActivatedRasterOverlay::processPendingUploads(
    bool interactionActive,
    FrameResourceBudget* budget) {
    syncProviderOptionsFromOverlay();
    if (tileProvider_) {
        return tileProvider_->processPendingUploads(interactionActive, budget);
    }
    return {};
}

bool ActivatedRasterOverlay::hasPendingWork() const {
    return tileProvider_ && tileProvider_->hasPendingWork();
}

uint64_t ActivatedRasterOverlay::revision() const {
    return tileProvider_ ? tileProvider_->revision() : 0;
}

void ActivatedRasterOverlay::setFrameNumber(uint64_t frameNumber) {
    syncProviderOptionsFromOverlay();
    if (tileProvider_) {
        tileProvider_->setFrameNumber(frameNumber);
    }
}

void ActivatedRasterOverlay::trimUnusedTiles(bool cachePressure) {
    syncProviderOptionsFromOverlay();
    if (tileProvider_) {
        tileProvider_->trimUnusedTiles(cachePressure);
    }
}

int64_t ActivatedRasterOverlay::tileTextureBytesUsed() const {
    return tileProvider_ ? tileProvider_->tileTextureBytesUsed() : 0;
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

void ActivatedRasterOverlay::syncProviderOptionsFromOverlay() {
    maximumSimultaneousTileLoads_ =
        overlay_.getOptions().maximumSimultaneousTileLoads > 0
            ? overlay_.getOptions().maximumSimultaneousTileLoads
            : 20;
    if (placeholderProvider_) {
        placeholderProvider_->maximumSimultaneousTileLoads =
            maximumSimultaneousTileLoads_;
        placeholderProvider_->applyOwnerOptions();
    }
    if (tileProvider_) {
        tileProvider_->maximumSimultaneousTileLoads =
            maximumSimultaneousTileLoads_;
        tileProvider_->applyOwnerOptions();
    }
}

} // namespace earth_engine
