#include "AmapVectorSource.h"

#include <cmath>

namespace earth_engine {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;

} // namespace

Rectangle amapTileRectangle(const TileKey& key) {
    const double n = std::exp2(key.z);
    const double westDeg = static_cast<double>(key.x) / n * 360.0 - 180.0;
    const double eastDeg = static_cast<double>(key.x + 1) / n * 360.0 - 180.0;
    const double northDeg = 90.0 - static_cast<double>(key.y) / n * 180.0;
    const double southDeg =
        90.0 - static_cast<double>(key.y + 1) / n * 180.0;
    return Rectangle(westDeg * kDegToRad, southDeg * kDegToRad,
                     eastDeg * kDegToRad, northDeg * kDegToRad);
}

} // namespace earth_engine
