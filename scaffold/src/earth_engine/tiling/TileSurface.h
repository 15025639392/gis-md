#pragma once

#include "../core/math/Rectangle.h"

namespace earth_engine {

/// Raster-overlay texture window (translation/scale in overlay UV space).
struct TileTextureWindow {
    float offsetU = 0.0f;
    float offsetV = 0.0f;
    float scaleU = 1.0f;
    float scaleV = 1.0f;
};

/// CPU-side helpers for mapping raster-overlay tiles onto geometry tiles.
class TileSurface {
public:
    // cesium-native: RasterOverlayUtilities::computeTranslationAndScale.
    // Returns translation/scale in projected overlay UV space, where V grows
    // from the projection rectangle's minimumY (south) to maximumY (north).
    // Does NOT embed tile-size or edge-bleed assumptions — those are handled
    // by the rendering layer (CLAMP_TO_EDGE + optional padding).
    static TileTextureWindow computeTranslationAndScale(const Rectangle& geometryBounds,
                                                         const Rectangle& imageryBounds);

    /// Convert native projected-overlay translation/scale to this renderer's
    /// current mesh UV convention: V=0 at the north edge and V=1 at the south.
    static TileTextureWindow textureWindowForNorthWestUv(
        const TileTextureWindow& nativeWindow);
};

} // namespace earth_engine
