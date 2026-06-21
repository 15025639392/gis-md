#pragma once

#include "RasterMappedToTilesetTile.h"
#include "../providers/RasterOverlayTile.h"

#include <memory>

namespace earth_engine {

class ActivatedRasterOverlay;

enum class SurfaceRasterBindingKind {
    None,
    RealTile,
    AncestorTile
};

struct SurfaceRasterBinding {
    SurfaceRasterBindingKind kind = SurfaceRasterBindingKind::None;
    const RasterOverlayTile* tile = nullptr;
    std::shared_ptr<RasterOverlayTile> tileHandle;
    float offsetU = 0.0f;
    float offsetV = 0.0f;
    float scaleU = 1.0f;
    float scaleV = 1.0f;
};

/// Core contract for drawable surface raster imagery.
///
/// A drawable surface raster must be the mapped ready tile, and that tile must
/// be a real Loaded/Done raster tile with an owned texture. Renderer fallback
/// textures, placeholders, failed tiles, and no-texture tiles are deliberately
/// outside this decision.
SurfaceRasterBinding chooseSurfaceRasterBinding(
    const RasterMappedToTilesetTile* mapped);

bool isLegalSurfaceRasterTile(const RasterOverlayTile* tile);

bool rasterOverlayBindingAllowedByPolicy(
    const ActivatedRasterOverlay* activeOverlay,
    const RasterMappedToTilesetTile* mapped,
    const SurfaceRasterBinding& binding);

} // namespace earth_engine
