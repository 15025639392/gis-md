#include "Rectangle.h"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>

namespace earth_engine {

namespace {
    constexpr double kRadToDeg = 180.0 / glm::pi<double>();
    constexpr double kDegToRad = glm::pi<double>() / 180.0;
}

Rectangle Rectangle::fromDegrees(double westDeg, double southDeg,
                                  double eastDeg, double northDeg) {
    return Rectangle(westDeg * kDegToRad, southDeg * kDegToRad,
                     eastDeg * kDegToRad, northDeg * kDegToRad);
}

double Rectangle::westDegrees() const { return west_ * kRadToDeg; }
double Rectangle::southDegrees() const { return south_ * kRadToDeg; }
double Rectangle::eastDegrees() const { return east_ * kRadToDeg; }
double Rectangle::northDegrees() const { return north_ * kRadToDeg; }

double Rectangle::width() const {
    if (crossesAntimeridian()) {
        return (glm::two_pi<double>() - west_) + east_;
    }
    return east_ - west_;
}

double Rectangle::height() const { return north_ - south_; }

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
    if (south_ > other.north_ || north_ < other.south_) return false;
    if (crossesAntimeridian() || other.crossesAntimeridian()) {
        // 保守处理跨反经线：任一跨则认为是相交（精细逻辑在 TileScheme 层）
        return true;
    }
    return west_ <= other.east_ && east_ >= other.west_;
}

bool Rectangle::crossesAntimeridian() const {
    return west_ > east_;
}

bool Rectangle::operator==(const Rectangle& rhs) const {
    return west_ == rhs.west_ && south_ == rhs.south_ &&
           east_ == rhs.east_ && north_ == rhs.north_;
}

bool Rectangle::operator!=(const Rectangle& rhs) const { return !(*this == rhs); }

std::ostream& operator<<(std::ostream& os, const Rectangle& r) {
    return os << "Rectangle(w:" << r.westDegrees() << "°, s:" << r.southDegrees()
              << "°, e:" << r.eastDegrees() << "°, n:" << r.northDegrees() << "°)";
}

} // namespace earth_engine
