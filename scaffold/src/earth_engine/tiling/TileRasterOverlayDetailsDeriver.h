#pragma once

#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Projection.h"
#include "../core/math/MathUtils.h"
#include "SurfaceTile.h"

#include <algorithm>
#include <cstddef>

namespace earth_engine {

class TileRasterOverlayDetailsDeriver {
public:
    static RasterOverlayDetails deriveChildFromParent(
        const RasterOverlayDetails& parentDetails,
        const Rectangle& parentBounds,
        const Rectangle& childBounds,
        double minimumHeight,
        double maximumHeight) {
        RasterOverlayDetails childDetails;
        if (parentDetails.empty()) {
            childDetails.setGeographicRectangle(
                childBounds,
                minimumHeight,
                maximumHeight);
            return childDetails;
        }

        childDetails.rasterOverlayProjections =
            parentDetails.rasterOverlayProjections;
        childDetails.rasterOverlayRectangles.reserve(
            parentDetails.rasterOverlayProjections.size());

        for (size_t i = 0;
             i < parentDetails.rasterOverlayProjections.size();
             ++i) {
            if (i >= parentDetails.rasterOverlayRectangles.size() ||
                parentDetails.rasterOverlayRectangles[i].isEmpty()) {
                childDetails.rasterOverlayRectangles.push_back(
                    Rectangle::EMPTY);
                continue;
            }

            const Rectangle& parentOverlay =
                parentDetails.rasterOverlayRectangles[i];
            const RasterOverlayProjection projection =
                parentDetails.rasterOverlayProjections[i];
            const Rectangle projectedParentBounds =
                projectBounds(parentBounds, projection);
            const Rectangle projectedChildBounds =
                projectBounds(childBounds, projection);
            const double childWestT = relativeX(
                projectedParentBounds,
                projectedChildBounds.west());
            const double childEastT = relativeX(
                projectedParentBounds,
                projectedChildBounds.east());
            const double childSouthT = relativeY(
                projectedParentBounds,
                projectedChildBounds.south());
            const double childNorthT = relativeY(
                projectedParentBounds,
                projectedChildBounds.north());
            childDetails.rasterOverlayRectangles.push_back(Rectangle(
                mixHorizontal(parentOverlay, projection, childWestT),
                mix(parentOverlay.south(), parentOverlay.north(), childSouthT),
                mixHorizontal(parentOverlay, projection, childEastT),
                mix(parentOverlay.south(), parentOverlay.north(), childNorthT)));
        }

        childDetails.boundingRegion = {
            childBounds,
            minimumHeight,
            maximumHeight};
        return childDetails;
    }

private:
    static Rectangle projectBounds(const Rectangle& bounds,
                                   RasterOverlayProjection projection) {
        switch (projection) {
            case RasterOverlayProjection::Geographic:
                return bounds;
            case RasterOverlayProjection::WebMercator:
                return projectRectangleSimple(
                    WebMercatorProjection(Ellipsoid::WGS84()),
                    bounds);
        }
        return bounds;
    }

    static double relativeX(const Rectangle& parentBounds, double x) {
        double offset = x - parentBounds.west();
        if (parentBounds.crossesAntimeridian() && offset < 0.0) {
            offset += MathUtils::TwoPi;
        }
        return std::clamp(offset / parentBounds.width(), 0.0, 1.0);
    }

    static double relativeY(const Rectangle& parentBounds, double y) {
        return std::clamp(
            (y - parentBounds.south()) / parentBounds.height(),
            0.0,
            1.0);
    }

    static double mix(double a, double b, double t) {
        return a + (b - a) * t;
    }

    static double mixHorizontal(const Rectangle& rectangle,
                                RasterOverlayProjection projection,
                                double t) {
        double east = rectangle.east();
        if (projection == RasterOverlayProjection::Geographic &&
            rectangle.crossesAntimeridian()) {
            east += MathUtils::TwoPi;
            return MathUtils::convertLongitudeRange(
                mix(rectangle.west(), east, t));
        }
        return mix(rectangle.west(), east, t);
    }
};

} // namespace earth_engine
