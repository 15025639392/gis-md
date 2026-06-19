#include "Cartographic.h"

#include <glm/gtc/constants.hpp>

namespace earth_engine {

namespace {
constexpr double kRadToDeg = 180.0 / glm::pi<double>();
constexpr double kDegToRad = glm::pi<double>() / 180.0;
}

Cartographic Cartographic::fromDegrees(double lngDeg,
                                       double latDeg,
                                       double heightM) {
    return Cartographic(lngDeg * kDegToRad, latDeg * kDegToRad, heightM);
}

double Cartographic::longitudeDegrees() const { return lng_ * kRadToDeg; }
double Cartographic::latitudeDegrees() const { return lat_ * kRadToDeg; }

bool Cartographic::operator==(const Cartographic& rhs) const {
    return lng_ == rhs.lng_ && lat_ == rhs.lat_ && height_ == rhs.height_;
}

bool Cartographic::operator!=(const Cartographic& rhs) const {
    return !(*this == rhs);
}

std::ostream& operator<<(std::ostream& os, const Cartographic& c) {
    return os << "Cartographic(lng:" << c.longitudeDegrees() << "°, lat:"
              << c.latitudeDegrees() << "°, h:" << c.height() << "m)";
}

} // namespace earth_engine
