#include "RasterResolution.h"

#include "DirectRasterMapping.h"
#include "SurfaceRasterBinding.h"

namespace earth_engine {

DirectRasterBindingResolution resolveDirectRasterBinding(
    const DirectRasterMapping* mapped) {
    DirectRasterBindingResolution bindingResult;
    RasterResolution& result = bindingResult.resolution;
    if (!mapped) {
        return bindingResult;
    }

    switch (mapped->getState()) {
        case DirectRasterMapping::State::Unattached:
            result.attachment = RasterAttachmentState::Unattached;
            break;
        case DirectRasterMapping::State::TemporarilyAttached:
            result.attachment = RasterAttachmentState::TemporarilyAttached;
            break;
        case DirectRasterMapping::State::Attached:
            result.attachment = RasterAttachmentState::Attached;
            break;
    }
    result.hasCoverage = !mapped->getDirectCompositeSourceTiles().empty();
    result.coverReady = mapped->getReadyTile() != nullptr;
    result.targetPending = mapped->hasPendingNonPlaceholderLoadingTile();
    result.targetFailed = mapped->didOriginalTargetFail();

    const RasterOverlayTile* ready = mapped->getReadyTile();
    const RasterOverlayTile* loading = mapped->getLoadingTile();
    const RasterOverlayTile* requestTile = loading ? loading : ready;
    if (requestTile) {
        if (requestTile->isEmptyComposition()) {
            result.requestState = RasterRequestState::Empty;
            result.targetFailed = true;
        } else {
            switch (requestTile->getState()) {
                case RasterOverlayTile::LoadState::Placeholder:
                    result.requestState = RasterRequestState::Placeholder;
                    break;
                case RasterOverlayTile::LoadState::Unloaded:
                    result.requestState = RasterRequestState::Unloaded;
                    break;
                case RasterOverlayTile::LoadState::Loading:
                    result.requestState = RasterRequestState::Loading;
                    break;
                case RasterOverlayTile::LoadState::Loaded:
                case RasterOverlayTile::LoadState::Done:
                    result.requestState = RasterRequestState::Loaded;
                    break;
                case RasterOverlayTile::LoadState::Failed:
                    result.requestState = RasterRequestState::Failed;
                    result.targetFailed = true;
                    break;
            }
        }
        if (requestTile->getState() != RasterOverlayTile::LoadState::Placeholder) {
            result.desiredZoom = requestTile->getTileID().z;
        }
    }
    if (mapped->desiredSourceZoom() >= 0) {
        result.desiredZoom = mapped->desiredSourceZoom();
    }
    if (mapped->didOriginalTargetFail() &&
        mapped->wasOriginalTargetEmpty()) {
        result.requestState = RasterRequestState::Empty;
    } else if (mapped->didOriginalTargetFail()) {
        result.requestState = RasterRequestState::Failed;
    }
    if (ready) {
        if (ready->getState() == RasterOverlayTile::LoadState::Failed ||
            ready->isEmptyComposition()) {
            result.targetFailed = true;
        } else {
            result.contentForm = ready->isDirectCompositeTile()
                ? RasterContentForm::Composed
                : RasterContentForm::Exact;
            result.sourceRelation =
                mapped->getReadyTileSource() ==
                        DirectRasterMapping::ReadyTileSource::Ancestor
                    ? RasterSourceRelation::Ancestor
                    : RasterSourceRelation::Own;
            result.resolvedZoom = ready->getTileID().z;
        }
    }

    const SurfaceRasterBinding binding = chooseSurfaceRasterBinding(mapped);
    result.drawable = binding.kind != SurfaceRasterBindingKind::None;
    bindingResult.sample.texture =
        binding.tile ? binding.tile->getTexture() : nullptr;
    bindingResult.sample.textureCoordinateId =
        mapped->getTextureCoordinateID();
    bindingResult.sample.offsetU = binding.offsetU;
    bindingResult.sample.offsetV = binding.offsetV;
    bindingResult.sample.scaleU = binding.scaleU;
    bindingResult.sample.scaleV = binding.scaleV;
    bindingResult.sample.resourceLease = binding.tileHandle;
    return bindingResult;
}

RasterResolution resolveDirectRasterResolution(
    const DirectRasterMapping* mapped) {
    return resolveDirectRasterBinding(mapped).resolution;
}

bool rasterResolutionAllowedByPolicy(const RasterResolution& resolution) {
    if (!resolution.visible || resolution.opacity <= 0.0f ||
        !resolution.drawable) {
        return false;
    }
    if (resolution.role == RasterOverlayRole::BaseImagery) {
        return true;
    }
    if (resolution.fallbackPolicy ==
        RasterOverlayFallbackPolicy::SkipUntilReady) {
        return !resolution.targetPending;
    }
    return true;
}

} // namespace earth_engine
