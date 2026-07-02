#include "TileSurface.h"

namespace earth_engine {

// cesium-native: RasterOverlayUtilities::computeTranslationAndScale
// geometryRectangle vs overlayRectangle, both in projected coordinates.
// For EPSG:4326 projected = rad * R, so ratio math is identical to radians.
// This function does NOT embed tile-size assumptions — edge bleed UV
// adjustment is handled by the rendering layer via CLAMP_TO_EDGE.
TileTextureWindow TileSurface::computeTranslationAndScale(
    const Rectangle& geometryBounds,
    const Rectangle& imageryBounds) {
    const double imgWidth = imageryBounds.width();
    const double imgHeight = imageryBounds.height();
    if (imgWidth <= 0.0 || imgHeight <= 0.0) return {};

    const double geoWidth = geometryBounds.width();
    const double geoHeight = geometryBounds.height();
    if (geoWidth <= 0.0 || geoHeight <= 0.0) return {};

    TileTextureWindow window;
    window.offsetU = static_cast<float>(
        (geometryBounds.west() - imageryBounds.west()) / imgWidth);
    window.scaleU = static_cast<float>(geoWidth / imgWidth);
    window.offsetV = static_cast<float>(
        (geometryBounds.south() - imageryBounds.south()) / imgHeight);
    window.scaleV = static_cast<float>(geoHeight / imgHeight);

    return window;
}

TileTextureWindow TileSurface::textureWindowForNorthWestUv(
    const TileTextureWindow& nativeWindow) {
    TileTextureWindow window = nativeWindow;
    window.offsetV = 1.0f - nativeWindow.offsetV - nativeWindow.scaleV;
    return window;
}

} // namespace earth_engine
