#include "SurfaceRasterBinding.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../layers/RasterOverlay.h"
#include "RasterOverlayRuntime.h"

namespace earth_engine {

bool isLegalSurfaceRasterTile(const RasterOverlayTile* tile) {
    if (!tile || !tile->getTexture()) {
        return false;
    }
    const RasterOverlayTile::LoadState state = tile->getState();
    return state == RasterOverlayTile::LoadState::Loaded ||
           state == RasterOverlayTile::LoadState::Done;
}

SurfaceRasterBinding chooseSurfaceRasterBinding(
    const DirectRasterMapping* mapped) {
    if (!mapped) {
        return {};
    }

    const RasterOverlayTile* readyTile = mapped->getReadyTile();
    if (!isLegalSurfaceRasterTile(readyTile)) {
        return {};
    }

    SurfaceRasterBinding binding;
    binding.tile = readyTile;
    binding.tileHandle = mapped->getReadyTileHandle();
    binding.offsetU = mapped->getTranslationU();
    binding.offsetV = mapped->getTranslationV();
    binding.scaleU = mapped->getScaleU();
    binding.scaleV = mapped->getScaleV();
    binding.kind = mapped->getReadyTileSource() ==
            DirectRasterMapping::ReadyTileSource::Ancestor
        ? SurfaceRasterBindingKind::AncestorTile
        : SurfaceRasterBindingKind::RealTile;
    return binding;
}

bool rasterOverlayBindingAllowedByPolicy(
    const ActivatedRasterOverlay* activeOverlay,
    const DirectRasterMapping* mapped,
    const SurfaceRasterBinding& binding) {
    if (!activeOverlay || !activeOverlay->visible() ||
        !mapped || binding.kind == SurfaceRasterBindingKind::None ||
        !binding.tile || !binding.tile->getTexture()) {
        return false;
    }
    if (activeOverlay->role() == RasterOverlayRole::BaseImagery) {
        return true;
    }
    if (activeOverlay->fallbackPolicy() ==
        RasterOverlayFallbackPolicy::SkipUntilReady) {
        return mapped->getLoadingTile() == nullptr;
    }
    return true;
}

bool rasterOverlayBindingAllowedByPolicy(
    const RasterOverlayFrameSlot& slot,
    const DirectRasterMapping* mapped,
    const SurfaceRasterBinding& binding) {
    if (!slot.visible || slot.opacity <= 0.0f || !mapped ||
        binding.kind == SurfaceRasterBindingKind::None ||
        !binding.tile || !binding.tile->getTexture()) {
        return false;
    }
    if (slot.role == RasterOverlayRole::BaseImagery) {
        return true;
    }
    if (slot.fallbackPolicy == RasterOverlayFallbackPolicy::SkipUntilReady) {
        return mapped->getLoadingTile() == nullptr;
    }
    return true;
}

} // namespace earth_engine
