#pragma once

#include "../core/geodesy/Cartographic.h"
#include "../core/math/Rectangle.h"
#include "../core/math/Vec3.h"

namespace earth_engine {

enum class RasterOverlayProjection {
    Geographic = 0,
    WebMercator = 1,
    Gcj02WebMercator = 2
};

enum class RasterOverlayGeoreference {
    Standard = 0,
    Gcj02WebMercator = 1
};

bool isWebMercatorRasterOverlayProjection(
    RasterOverlayProjection projection);

/// Short stable name for logs/diagnostics ("geo" / "merc" / "gcj-merc").
const char* rasterOverlayProjectionName(RasterOverlayProjection projection);

/// Projects a WGS84 world position into the overlay's sampling space.
Vec3 projectWorldPositionForRasterOverlay(
    const Cartographic& cartographic,
    RasterOverlayProjection projection);

/// Bounds a WGS84 world rectangle in the overlay's sampling space.
Rectangle projectWorldRectangleForRasterOverlay(
    const Rectangle& rectangle,
    RasterOverlayProjection projection);

/// Projects coordinates already expressed in the source datum. For a GCJ-02
/// source these coordinates are GCJ-02 longitude/latitude, not WGS84.
Rectangle projectRasterSourceRectangle(
    const Rectangle& rectangle,
    RasterOverlayProjection projection);

/// Converts an overlay-space rectangle back to source-datum longitude/latitude.
Rectangle unprojectRasterSourceRectangle(
    const Rectangle& rectangle,
    RasterOverlayProjection projection);

/// Converts a WGS84 coverage rectangle to source-datum longitude/latitude.
Rectangle worldRectangleToRasterSource(
    const Rectangle& rectangle,
    RasterOverlayProjection projection);

} // namespace earth_engine
