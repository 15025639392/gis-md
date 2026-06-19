#include "Projection.h"

#include <algorithm>
#include <cmath>
#include <glm/common.hpp>

namespace earth_engine {

Vec3 projectPosition(const Projection& projection, const Cartographic& position) {
    struct Operation {
        const Cartographic& position;

        Vec3 operator()(const GeographicProjection& geographic) const {
            return geographic.project(position);
        }

        Vec3 operator()(const WebMercatorProjection& webMercator) const {
            return webMercator.project(position);
        }
    };

    return std::visit(Operation{position}, projection);
}

Cartographic unprojectPosition(const Projection& projection, const Vec3& position) {
    struct Operation {
        const Vec3& position;

        Cartographic operator()(const GeographicProjection& geographic) const {
            return geographic.unproject(position);
        }

        Cartographic operator()(const WebMercatorProjection& webMercator) const {
            return webMercator.unproject(position);
        }
    };

    return std::visit(Operation{position}, projection);
}

Rectangle projectRectangleSimple(const Projection& projection,
                                 const Rectangle& rectangle) {
    struct Operation {
        const Rectangle& rectangle;

        Rectangle operator()(const GeographicProjection& geographic) const {
            return geographic.project(rectangle);
        }

        Rectangle operator()(const WebMercatorProjection& webMercator) const {
            return webMercator.project(rectangle);
        }
    };

    return std::visit(Operation{rectangle}, projection);
}

Rectangle unprojectRectangleSimple(const Projection& projection,
                                   const Rectangle& rectangle) {
    struct Operation {
        const Rectangle& rectangle;

        Rectangle operator()(const GeographicProjection& geographic) const {
            return geographic.unproject(rectangle);
        }

        Rectangle operator()(const WebMercatorProjection& webMercator) const {
            return webMercator.unproject(rectangle);
        }
    };

    return std::visit(Operation{rectangle}, projection);
}

glm::dvec2 computeProjectedRectangleSize(const Projection& projection,
                                         const Rectangle& rectangle,
                                         double maxHeight,
                                         const Ellipsoid& ellipsoid) {
    auto cartesianAt = [&](double x, double y) {
        return ellipsoid.cartographicToCartesian(
            unprojectPosition(projection, Vec3(x, y, maxHeight)));
    };

    const Vec3 ll = cartesianAt(rectangle.west(), rectangle.south());
    const Vec3 lr = cartesianAt(rectangle.east(), rectangle.south());
    const Vec3 ul = cartesianAt(rectangle.west(), rectangle.north());
    const Vec3 ur = cartesianAt(rectangle.east(), rectangle.north());

    const double lowerDistance = ll.distanceTo(lr);
    const double upperDistance = ul.distanceTo(ur);
    const double leftDistance = ll.distanceTo(ul);
    const double rightDistance = lr.distanceTo(ur);

    double x = std::max(lowerDistance, upperDistance);
    double y = std::max(leftDistance, rightDistance);

    const auto center = rectangle.center();
    const Vec3 lc = cartesianAt(center.first, rectangle.south());
    const Vec3 uc = cartesianAt(center.first, rectangle.north());

    const double halfDistanceLA = ll.distanceTo(lc);
    const double halfDistanceLB = lc.distanceTo(lr);
    const double halfDistanceUA = ul.distanceTo(uc);
    const double halfDistanceUB = uc.distanceTo(ur);

    if (halfDistanceLA > x || halfDistanceLB > x ||
        halfDistanceUA > x || halfDistanceUB > x) {
        x = std::max(halfDistanceLA + halfDistanceLB,
                     halfDistanceUA + halfDistanceUB);
    }

    if (glm::sign(rectangle.west()) != glm::sign(rectangle.east())) {
        const Vec3 top = cartesianAt(0.0, rectangle.north());
        const Vec3 bottom = cartesianAt(0.0, rectangle.south());
        y = std::max(y, top.distanceTo(bottom));
    }

    if (glm::sign(rectangle.south()) != glm::sign(rectangle.north())) {
        const Vec3 left = cartesianAt(rectangle.west(), 0.0);
        const Vec3 right = cartesianAt(rectangle.east(), 0.0);
        const double distance = left.distanceTo(right);
        x = std::max(x, distance);

        const Vec3 centerPoint = cartesianAt(center.first, 0.0);
        const double halfDistanceL = left.distanceTo(centerPoint);
        const double halfDistanceR = centerPoint.distanceTo(right);
        if (halfDistanceL > x || halfDistanceR > x) {
            x = halfDistanceL + halfDistanceR;
        }
    }

    return glm::dvec2(x, y);
}

const Ellipsoid& getProjectionEllipsoid(const Projection& projection) {
    struct Operation {
        const Ellipsoid& operator()(const GeographicProjection& geographic) const {
            return geographic.ellipsoid();
        }

        const Ellipsoid& operator()(const WebMercatorProjection& webMercator) const {
            return webMercator.ellipsoid();
        }
    };

    return std::visit(Operation{}, projection);
}

} // namespace earth_engine
