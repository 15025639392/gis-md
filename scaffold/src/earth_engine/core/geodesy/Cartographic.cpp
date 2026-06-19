#include "Cartographic.h"
#include "../math/MathUtils.h"

namespace earth_engine {

Cartographic Cartographic::fromDegrees(double lngDeg,
                                       double latDeg,
                                       double heightM) {
    return Cartographic(MathUtils::degreesToRadians(lngDeg),
                        MathUtils::degreesToRadians(latDeg),
                        heightM);
}

double Cartographic::longitudeDegrees() const {
    return MathUtils::radiansToDegrees(lng_);
}

double Cartographic::latitudeDegrees() const {
    return MathUtils::radiansToDegrees(lat_);
}

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
