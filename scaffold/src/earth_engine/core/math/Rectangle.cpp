#include "Rectangle.h"
#include "MathUtils.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace earth_engine {

namespace {
    constexpr double kPi = MathUtils::OnePi;
    constexpr double kTwoPi = MathUtils::TwoPi;
}

const Rectangle Rectangle::EMPTY{kPi, kPi * 0.5, -kPi, -kPi * 0.5};
const Rectangle Rectangle::MAXIMUM{-kPi, -kPi * 0.5, kPi, kPi * 0.5};

Rectangle Rectangle::fromDegrees(double westDeg, double southDeg,
                                  double eastDeg, double northDeg) {
    return Rectangle(MathUtils::degreesToRadians(westDeg),
                     MathUtils::degreesToRadians(southDeg),
                     MathUtils::degreesToRadians(eastDeg),
                     MathUtils::degreesToRadians(northDeg));
}

double Rectangle::westDegrees() const { return MathUtils::radiansToDegrees(west_); }
double Rectangle::southDegrees() const { return MathUtils::radiansToDegrees(south_); }
double Rectangle::eastDegrees() const { return MathUtils::radiansToDegrees(east_); }
double Rectangle::northDegrees() const { return MathUtils::radiansToDegrees(north_); }

double Rectangle::width() const {
    if (crossesAntimeridian()) {
        return (kTwoPi - west_) + east_;
    }
    return east_ - west_;
}

double Rectangle::height() const { return north_ - south_; }

std::pair<double, double> Rectangle::center() const {
    const double latitudeCenter = (south_ + north_) * 0.5;

    if (!crossesAntimeridian()) {
        return {(west_ + east_) * 0.5, latitudeCenter};
    }

    const double westToAntimeridian = kPi - west_;
    const double antimeridianToEast = east_ - -kPi;
    const double total = westToAntimeridian + antimeridianToEast;
    if (westToAntimeridian >= antimeridianToEast) {
        return {std::min(kPi, west_ + total * 0.5), latitudeCenter};
    }
    return {std::max(-kPi, east_ - total * 0.5), latitudeCenter};
}

std::pair<double, double> Rectangle::normalizedCoordinates(double lngRad,
                                                           double latRad) const {
    double east = east_;
    double longitude = lngRad;
    if (east < west_) {
        east += kTwoPi;
        if (longitude < west_) {
            longitude += kTwoPi;
        }
    }

    return {
        (longitude - west_) / (east - west_),
        (latRad - south_) / (north_ - south_)
    };
}

bool Rectangle::contains(double lngRad, double latRad) const {
    if (latRad < south_ || latRad > north_) return false;
    if (crossesAntimeridian()) {
        return lngRad >= west_ || lngRad <= east_;
    }
    return lngRad >= west_ && lngRad <= east_;
}

bool Rectangle::contains(const Rectangle& other) const {
    return contains(other.west_, other.south_) &&
           contains(other.east_, other.north_);
}

bool Rectangle::intersects(const Rectangle& other) const {
    return computeIntersection(other).has_value();
}

bool Rectangle::isEmpty() const {
    return south_ > north_;
}

double Rectangle::computeSignedDistance(double lngRad, double latRad) const {
    const double westDistance = west_ - lngRad;
    const double southDistance = south_ - latRad;
    const double eastDistance = lngRad - east_;
    const double northDistance = latRad - north_;
    const double maxLongitudeDistance = std::max(westDistance, eastDistance);
    const double maxLatitudeDistance = std::max(southDistance, northDistance);

    if (maxLongitudeDistance <= 0.0 && maxLatitudeDistance <= 0.0) {
        return std::max(maxLongitudeDistance, maxLatitudeDistance);
    }
    if (maxLongitudeDistance > 0.0 && maxLatitudeDistance > 0.0) {
        return std::sqrt(maxLongitudeDistance * maxLongitudeDistance +
                         maxLatitudeDistance * maxLatitudeDistance);
    }
    return std::max(maxLongitudeDistance, maxLatitudeDistance);
}

std::optional<Rectangle> Rectangle::computeIntersection(const Rectangle& other) const {
    double rectangleEast = east_;
    double rectangleWest = west_;
    double otherRectangleEast = other.east_;
    double otherRectangleWest = other.west_;

    if (rectangleEast < rectangleWest && otherRectangleEast > 0.0) {
        rectangleEast += kTwoPi;
    } else if (otherRectangleEast < otherRectangleWest && rectangleEast > 0.0) {
        otherRectangleEast += kTwoPi;
    }

    if (rectangleEast < rectangleWest && otherRectangleWest < 0.0) {
        otherRectangleWest += kTwoPi;
    } else if (otherRectangleEast < otherRectangleWest && rectangleWest < 0.0) {
        rectangleWest += kTwoPi;
    }

    const double west = MathUtils::negativePiToPi(
        std::max(rectangleWest, otherRectangleWest));
    const double east = MathUtils::negativePiToPi(
        std::min(rectangleEast, otherRectangleEast));

    if ((west_ < east_ || other.west_ < other.east_) && east <= west) {
        return std::nullopt;
    }

    const double south = std::max(south_, other.south_);
    const double north = std::min(north_, other.north_);

    if (south >= north) {
        return std::nullopt;
    }

    return Rectangle(west, south, east, north);
}

Rectangle Rectangle::computeUnion(const Rectangle& other) const {
    double rectangleEast = east_;
    double rectangleWest = west_;
    double otherRectangleEast = other.east_;
    double otherRectangleWest = other.west_;

    if (rectangleEast < rectangleWest && otherRectangleEast > 0.0) {
        rectangleEast += kTwoPi;
    } else if (otherRectangleEast < otherRectangleWest && rectangleEast > 0.0) {
        otherRectangleEast += kTwoPi;
    }

    if (rectangleEast < rectangleWest && otherRectangleWest < 0.0) {
        otherRectangleWest += kTwoPi;
    } else if (otherRectangleEast < otherRectangleWest && rectangleWest < 0.0) {
        rectangleWest += kTwoPi;
    }

    double west = std::min(rectangleWest, otherRectangleWest);
    double east = std::max(rectangleEast, otherRectangleEast);

    if (west != kPi) {
        west = MathUtils::convertLongitudeRange(west);
    }
    if (east != kPi) {
        east = MathUtils::convertLongitudeRange(east);
    }

    return Rectangle(west, std::min(south_, other.south_),
                     east, std::max(north_, other.north_));
}

std::pair<Rectangle, std::optional<Rectangle>>
Rectangle::splitAtAntimeridian() const {
    if (!crossesAntimeridian()) {
        return {*this, std::nullopt};
    }

    Rectangle a(west_, south_, kPi, north_);
    Rectangle b(-kPi, south_, east_, north_);

    if (a.width() > b.width()) {
        return {a, b};
    }
    return {b, a};
}

bool Rectangle::crossesAntimeridian() const {
    return west_ > east_;
}

bool Rectangle::operator==(const Rectangle& rhs) const {
    return west_ == rhs.west_ && south_ == rhs.south_ &&
           east_ == rhs.east_ && north_ == rhs.north_;
}

bool Rectangle::operator!=(const Rectangle& rhs) const { return !(*this == rhs); }

bool Rectangle::equalsEpsilon(const Rectangle& rhs, double epsilon) const {
    return std::abs(west_ - rhs.west_) <= epsilon &&
           std::abs(south_ - rhs.south_) <= epsilon &&
           std::abs(east_ - rhs.east_) <= epsilon &&
           std::abs(north_ - rhs.north_) <= epsilon;
}

std::ostream& operator<<(std::ostream& os, const Rectangle& r) {
    return os << "Rectangle(w:" << r.westDegrees() << "°, s:" << r.southDegrees()
              << "°, e:" << r.eastDegrees() << "°, n:" << r.northDegrees() << "°)";
}

} // namespace earth_engine
