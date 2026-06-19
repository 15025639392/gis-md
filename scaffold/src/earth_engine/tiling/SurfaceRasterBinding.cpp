#include "SurfaceRasterBinding.h"

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

} // namespace earth_engine
