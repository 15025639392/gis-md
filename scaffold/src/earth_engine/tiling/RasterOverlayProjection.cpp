#include "RasterOverlayProjection.h"

#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Gcj02CoordinateTransform.h"
#include "../core/geodesy/Projection.h"
#include "../core/geodesy/WebMercatorProjection.h"
#include "../core/math/MathUtils.h"

#include <algorithm>

namespace earth_engine {
namespace {

Rectangle projectWebMercatorRectangle(const Rectangle& rectangle) {
    return projectRectangleSimple(
        WebMercatorProjection(Ellipsoid::WGS84()),
        rectangle);
}

} // namespace

bool isWebMercatorRasterOverlayProjection(
    RasterOverlayProjection projection) {
    return projection == RasterOverlayProjection::WebMercator ||
           projection == RasterOverlayProjection::Gcj02WebMercator;
}

const char* rasterOverlayProjectionName(
    RasterOverlayProjection projection) {
    switch (projection) {
        case RasterOverlayProjection::Geographic:
            return "geo";
        case RasterOverlayProjection::WebMercator:
            return "merc";
        case RasterOverlayProjection::Gcj02WebMercator:
            return "gcj-merc";
    }
    return "?";
}

Vec3 projectWorldPositionForRasterOverlay(
    const Cartographic& cartographic,
    RasterOverlayProjection projection) {
    switch (projection) {
        case RasterOverlayProjection::Geographic:
            return Vec3(
                cartographic.longitude(),
                cartographic.latitude(),
                cartographic.height());
        case RasterOverlayProjection::WebMercator:
            return projectPosition(
                WebMercatorProjection(Ellipsoid::WGS84()),
                cartographic);
        case RasterOverlayProjection::Gcj02WebMercator:
            return projectPosition(
                WebMercatorProjection(Ellipsoid::WGS84()),
                Gcj02CoordinateTransform::fromWgs84(cartographic));
    }
    return Vec3(
        cartographic.longitude(),
        cartographic.latitude(),
        cartographic.height());
}

Rectangle projectWorldRectangleForRasterOverlay(
    const Rectangle& rectangle,
    RasterOverlayProjection projection) {
    if (projection == RasterOverlayProjection::Geographic) {
        return rectangle;
    }
    if (projection == RasterOverlayProjection::WebMercator) {
        return projectWebMercatorRectangle(rectangle);
    }
    return projectWebMercatorRectangle(
        Gcj02CoordinateTransform::boundRectangleFromWgs84(rectangle));
}

Rectangle projectRasterSourceRectangle(
    const Rectangle& rectangle,
    RasterOverlayProjection projection) {
    return isWebMercatorRasterOverlayProjection(projection)
        ? projectWebMercatorRectangle(rectangle)
        : rectangle;
}

Rectangle unprojectRasterSourceRectangle(
    const Rectangle& rectangle,
    RasterOverlayProjection projection) {
    return isWebMercatorRasterOverlayProjection(projection)
        ? unprojectRectangleSimple(
              WebMercatorProjection(Ellipsoid::WGS84()),
              rectangle)
        : rectangle;
}

Rectangle worldRectangleToRasterSource(
    const Rectangle& rectangle,
    RasterOverlayProjection projection) {
    if (projection != RasterOverlayProjection::Gcj02WebMercator) {
        return rectangle;
    }

    const Rectangle transformed =
        Gcj02CoordinateTransform::boundRectangleFromWgs84(rectangle);
    return Rectangle(
        std::max(-MathUtils::OnePi, transformed.west()),
        std::max(-MathUtils::PiOverTwo, transformed.south()),
        std::min(MathUtils::OnePi, transformed.east()),
        std::min(MathUtils::PiOverTwo, transformed.north()));
}

} // namespace earth_engine
