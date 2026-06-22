#pragma once

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

        const double parentWidth = parentBounds.width();
        const double parentHeight = parentBounds.height();
        const auto relativeLongitude = [&](double longitude) {
            double offset = longitude - parentBounds.west();
            if (parentBounds.crossesAntimeridian() && offset < 0.0) {
                offset += MathUtils::TwoPi;
            }
            return std::clamp(offset / parentWidth, 0.0, 1.0);
        };
        const double childWestT = relativeLongitude(childBounds.west());
        const double childEastT = relativeLongitude(childBounds.east());
        const double childSouthT =
            std::clamp((childBounds.south() - parentBounds.south()) /
                           parentHeight,
                       0.0,
                       1.0);
        const double childNorthT =
            std::clamp((childBounds.north() - parentBounds.south()) /
                           parentHeight,
                       0.0,
                       1.0);

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
            childDetails.rasterOverlayRectangles.push_back(Rectangle(
                mix(parentOverlay.west(), parentOverlay.east(), childWestT),
                mix(parentOverlay.south(), parentOverlay.north(), childSouthT),
                mix(parentOverlay.west(), parentOverlay.east(), childEastT),
                mix(parentOverlay.south(), parentOverlay.north(), childNorthT)));
        }

        childDetails.boundingRegion = {
            childBounds,
            minimumHeight,
            maximumHeight};
        return childDetails;
    }

private:
    static double mix(double a, double b, double t) {
        return a + (b - a) * t;
    }
};

} // namespace earth_engine
