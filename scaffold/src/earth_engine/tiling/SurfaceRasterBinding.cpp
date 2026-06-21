#include "SurfaceRasterBinding.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../layers/RasterOverlay.h"

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
    const RasterMappedToTilesetTile* mapped) {
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
            RasterMappedToTilesetTile::ReadyTileSource::Ancestor
        ? SurfaceRasterBindingKind::AncestorTile
        : SurfaceRasterBindingKind::RealTile;
    return binding;
}

bool rasterOverlayBindingAllowedByPolicy(
    const ActivatedRasterOverlay* activeOverlay,
    const RasterMappedToTilesetTile* mapped,
    const SurfaceRasterBinding& binding) {
    if (!activeOverlay || !activeOverlay->visible() ||
        !mapped || binding.kind == SurfaceRasterBindingKind::None ||
        !binding.tile || !binding.tile->getTexture()) {
        return false;
    }
    if (activeOverlay->getOverlay().role() ==
            RasterOverlayRole::BaseImagery) {
        return true;
    }
    if (activeOverlay->getOverlay().fallbackPolicy() ==
        RasterOverlayFallbackPolicy::SkipUntilReady) {
        return mapped->getLoadingTile() == nullptr;
    }
    return true;
}

} // namespace earth_engine
