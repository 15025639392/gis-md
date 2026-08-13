#include "Gcj02CoordinateTransform.h"

#include "../math/MathUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace earth_engine {
namespace {

constexpr double kSemimajorAxis = 6378245.0;
constexpr double kEccentricitySquared = 0.00669342162296594323;
constexpr double kDegreesToRadians =
    0.017453292519943295769236907684886;
constexpr double kRadiansToDegrees =
    57.295779513082320876798154814105;
constexpr double kChinaWestDegrees = 72.004;
constexpr double kChinaSouthDegrees = 0.8293;
constexpr double kChinaEastDegrees = 137.8347;
constexpr double kChinaNorthDegrees = 55.8271;
constexpr double kChinaWest = kChinaWestDegrees * kDegreesToRadians;
constexpr double kChinaSouth = kChinaSouthDegrees * kDegreesToRadians;
constexpr double kChinaEast = kChinaEastDegrees * kDegreesToRadians;
constexpr double kChinaNorth = kChinaNorthDegrees * kDegreesToRadians;
constexpr int kRectangleIntervalPartitions = 8;

double roundDown(double value) {
    return std::nextafter(
        value,
        -std::numeric_limits<double>::infinity());
}

double roundUp(double value) {
    return std::nextafter(
        value,
        std::numeric_limits<double>::infinity());
}

struct Interval {
    Interval(double value) : lower(value), upper(value) {}
    Interval(double lower_, double upper_) : lower(lower_), upper(upper_) {}

    double lower;
    double upper;
};

Interval operator+(const Interval& lhs, const Interval& rhs) {
    return Interval(
        roundDown(lhs.lower + rhs.lower),
        roundUp(lhs.upper + rhs.upper));
}

Interval operator-(const Interval& lhs, const Interval& rhs) {
    return Interval(
        roundDown(lhs.lower - rhs.upper),
        roundUp(lhs.upper - rhs.lower));
}

Interval operator*(const Interval& lhs, const Interval& rhs) {
    const std::array<double, 4> products{
        lhs.lower * rhs.lower,
        lhs.lower * rhs.upper,
        lhs.upper * rhs.lower,
        lhs.upper * rhs.upper};
    const auto [minimum, maximum] =
        std::minmax_element(products.begin(), products.end());
    return Interval(roundDown(*minimum), roundUp(*maximum));
}

Interval operator/(const Interval& lhs, const Interval& rhs) {
    return lhs * Interval(
        roundDown(1.0 / rhs.upper),
        roundUp(1.0 / rhs.lower));
}

Interval square(const Interval& value) {
    if (value.lower <= 0.0 && value.upper >= 0.0) {
        return Interval(
            0.0,
            roundUp(std::max(
                value.lower * value.lower,
                value.upper * value.upper)));
    }
    const double lowerSquared = value.lower * value.lower;
    const double upperSquared = value.upper * value.upper;
    return Interval(
        roundDown(std::min(lowerSquared, upperSquared)),
        roundUp(std::max(lowerSquared, upperSquared)));
}

Interval sqrtInterval(const Interval& value) {
    return Interval(
        roundDown(std::sqrt(std::max(0.0, value.lower))),
        roundUp(std::sqrt(std::max(0.0, value.upper))));
}

Interval sqrtAbsolute(const Interval& value) {
    const double maximumAbsolute =
        std::max(std::abs(value.lower), std::abs(value.upper));
    const double minimumAbsolute =
        value.lower <= 0.0 && value.upper >= 0.0
            ? 0.0
            : std::min(std::abs(value.lower), std::abs(value.upper));
    return Interval(
        roundDown(std::sqrt(minimumAbsolute)),
        roundUp(std::sqrt(maximumAbsolute)));
}

bool containsPeriodicPoint(const Interval& interval,
                           double phase,
                           double period) {
    const double first =
        std::ceil((interval.lower - phase) / period);
    const double last =
        std::floor((interval.upper - phase) / period);
    return first <= last;
}

Interval sinInterval(const Interval& value) {
    if (value.upper - value.lower >= MathUtils::TwoPi) {
        return Interval(-1.0, 1.0);
    }

    double lower = std::min(
        std::sin(value.lower),
        std::sin(value.upper));
    double upper = std::max(
        std::sin(value.lower),
        std::sin(value.upper));
    if (containsPeriodicPoint(
            value,
            -MathUtils::PiOverTwo,
            MathUtils::TwoPi)) {
        lower = -1.0;
    } else {
        lower = roundDown(lower);
    }
    if (containsPeriodicPoint(
            value,
            MathUtils::PiOverTwo,
            MathUtils::TwoPi)) {
        upper = 1.0;
    } else {
        upper = roundUp(upper);
    }
    return Interval(lower, upper);
}

struct TransformedIntervals {
    Interval longitudeDegrees;
    Interval latitudeDegrees;
};

TransformedIntervals boundTransformedCell(
    double westDegrees,
    double southDegrees,
    double eastDegrees,
    double northDegrees) {
    const Interval longitudeDegrees(westDegrees, eastDegrees);
    const Interval latitudeDegrees(southDegrees, northDegrees);
    const Interval x = longitudeDegrees - Interval(105.0);
    const Interval y = latitudeDegrees - Interval(35.0);

    Interval latitudeDelta =
        Interval(-100.0) +
        Interval(2.0) * x +
        Interval(3.0) * y +
        Interval(0.2) * square(y) +
        Interval(0.1) * x * y +
        Interval(0.2) * sqrtAbsolute(x);
    latitudeDelta =
        latitudeDelta +
        (Interval(20.0) *
             sinInterval(Interval(6.0 * MathUtils::OnePi) * x) +
         Interval(20.0) *
             sinInterval(Interval(2.0 * MathUtils::OnePi) * x)) *
            Interval(2.0 / 3.0);
    latitudeDelta =
        latitudeDelta +
        (Interval(20.0) *
             sinInterval(Interval(MathUtils::OnePi) * y) +
         Interval(40.0) *
             sinInterval(Interval(MathUtils::OnePi / 3.0) * y)) *
            Interval(2.0 / 3.0);
    latitudeDelta =
        latitudeDelta +
        (Interval(160.0) *
             sinInterval(Interval(MathUtils::OnePi / 12.0) * y) +
         Interval(320.0) *
             sinInterval(Interval(MathUtils::OnePi / 30.0) * y)) *
            Interval(2.0 / 3.0);

    Interval longitudeDelta =
        Interval(300.0) +
        x +
        Interval(2.0) * y +
        Interval(0.1) * square(x) +
        Interval(0.1) * x * y +
        Interval(0.1) * sqrtAbsolute(x);
    longitudeDelta =
        longitudeDelta +
        (Interval(20.0) *
             sinInterval(Interval(6.0 * MathUtils::OnePi) * x) +
         Interval(20.0) *
             sinInterval(Interval(2.0 * MathUtils::OnePi) * x)) *
            Interval(2.0 / 3.0);
    longitudeDelta =
        longitudeDelta +
        (Interval(20.0) *
             sinInterval(Interval(MathUtils::OnePi) * x) +
         Interval(40.0) *
             sinInterval(Interval(MathUtils::OnePi / 3.0) * x)) *
            Interval(2.0 / 3.0);
    longitudeDelta =
        longitudeDelta +
        (Interval(150.0) *
             sinInterval(Interval(MathUtils::OnePi / 12.0) * x) +
         Interval(300.0) *
             sinInterval(Interval(MathUtils::OnePi / 30.0) * x)) *
            Interval(2.0 / 3.0);

    const Interval latitudeRadians =
        latitudeDegrees * Interval(kDegreesToRadians);
    const Interval sinLatitude = sinInterval(latitudeRadians);
    const Interval magic =
        Interval(1.0) -
        Interval(kEccentricitySquared) * square(sinLatitude);
    const Interval sqrtMagic = sqrtInterval(magic);
    const Interval latitudeFactor =
        Interval(
            180.0 /
            (kSemimajorAxis *
             (1.0 - kEccentricitySquared) *
             MathUtils::OnePi)) *
        magic *
        sqrtMagic;
    const Interval cosLatitude = sinInterval(
        latitudeRadians + Interval(MathUtils::PiOverTwo));
    const Interval longitudeFactor =
        Interval(
            180.0 /
            (kSemimajorAxis * MathUtils::OnePi)) *
        sqrtMagic /
        cosLatitude;

    return {
        longitudeDegrees + longitudeDelta * longitudeFactor,
        latitudeDegrees + latitudeDelta * latitudeFactor};
}

class RectangleBounds {
public:
    void include(const Rectangle& rectangle) {
        if (rectangle.isEmpty()) {
            return;
        }
        include(
            rectangle.west(),
            rectangle.south(),
            rectangle.east(),
            rectangle.north());
    }

    void include(double west, double south, double east, double north) {
        if (!hasValue_) {
            west_ = west;
            south_ = south;
            east_ = east;
            north_ = north;
            hasValue_ = true;
            return;
        }
        west_ = std::min(west_, west);
        south_ = std::min(south_, south);
        east_ = std::max(east_, east);
        north_ = std::max(north_, north);
    }

    Rectangle rectangle() const {
        return hasValue_
            ? Rectangle(west_, south_, east_, north_)
            : Rectangle::EMPTY;
    }

private:
    bool hasValue_ = false;
    double west_ = 0.0;
    double south_ = 0.0;
    double east_ = 0.0;
    double north_ = 0.0;
};

Rectangle boundInsideChina(const Rectangle& rectangle) {
    const double westDegrees = rectangle.west() * kRadiansToDegrees;
    const double southDegrees = rectangle.south() * kRadiansToDegrees;
    const double eastDegrees = rectangle.east() * kRadiansToDegrees;
    const double northDegrees = rectangle.north() * kRadiansToDegrees;
    const int longitudePartitions =
        eastDegrees > westDegrees ? kRectangleIntervalPartitions : 1;
    const int latitudePartitions =
        northDegrees > southDegrees ? kRectangleIntervalPartitions : 1;

    RectangleBounds result;
    for (int yIndex = 0; yIndex < latitudePartitions; ++yIndex) {
        const double cellSouth = yIndex == 0
            ? southDegrees
            : southDegrees +
                  (northDegrees - southDegrees) *
                      static_cast<double>(yIndex) /
                      static_cast<double>(latitudePartitions);
        const double cellNorth = yIndex + 1 == latitudePartitions
            ? northDegrees
            : southDegrees +
                  (northDegrees - southDegrees) *
                      static_cast<double>(yIndex + 1) /
                      static_cast<double>(latitudePartitions);
        for (int xIndex = 0; xIndex < longitudePartitions; ++xIndex) {
            const double cellWest = xIndex == 0
                ? westDegrees
                : westDegrees +
                      (eastDegrees - westDegrees) *
                          static_cast<double>(xIndex) /
                          static_cast<double>(longitudePartitions);
            const double cellEast = xIndex + 1 == longitudePartitions
                ? eastDegrees
                : westDegrees +
                      (eastDegrees - westDegrees) *
                          static_cast<double>(xIndex + 1) /
                          static_cast<double>(longitudePartitions);
            const TransformedIntervals transformed =
                boundTransformedCell(
                    cellWest,
                    cellSouth,
                    cellEast,
                    cellNorth);
            result.include(
                roundDown(
                    transformed.longitudeDegrees.lower *
                    kDegreesToRadians),
                roundDown(
                    transformed.latitudeDegrees.lower *
                    kDegreesToRadians),
                roundUp(
                    transformed.longitudeDegrees.upper *
                    kDegreesToRadians),
                roundUp(
                    transformed.latitudeDegrees.upper *
                    kDegreesToRadians));
        }
    }
    return result.rectangle();
}

Rectangle boundNonCrossingRectangle(const Rectangle& rectangle) {
    if (rectangle.isEmpty() ||
        rectangle.east() < kChinaWest ||
        rectangle.west() > kChinaEast ||
        rectangle.north() < kChinaSouth ||
        rectangle.south() > kChinaNorth) {
        return rectangle;
    }

    const Rectangle inside(
        std::max(rectangle.west(), kChinaWest),
        std::max(rectangle.south(), kChinaSouth),
        std::min(rectangle.east(), kChinaEast),
        std::min(rectangle.north(), kChinaNorth));
    if (inside == rectangle) {
        return boundInsideChina(inside);
    }

    RectangleBounds result;
    result.include(boundInsideChina(inside));

    if (rectangle.west() < kChinaWest) {
        result.include(Rectangle(
            rectangle.west(),
            rectangle.south(),
            std::min(rectangle.east(), kChinaWest),
            rectangle.north()));
    }
    if (rectangle.east() > kChinaEast) {
        result.include(Rectangle(
            std::max(rectangle.west(), kChinaEast),
            rectangle.south(),
            rectangle.east(),
            rectangle.north()));
    }

    const double middleWest =
        std::max(rectangle.west(), kChinaWest);
    const double middleEast =
        std::min(rectangle.east(), kChinaEast);
    if (rectangle.south() < kChinaSouth) {
        result.include(Rectangle(
            middleWest,
            rectangle.south(),
            middleEast,
            std::min(rectangle.north(), kChinaSouth)));
    }
    if (rectangle.north() > kChinaNorth) {
        result.include(Rectangle(
            middleWest,
            std::max(rectangle.south(), kChinaNorth),
            middleEast,
            rectangle.north()));
    }
    return result.rectangle();
}

double transformLatitude(double x, double y) {
    double result =
        -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y +
        0.1 * x * y + 0.2 * std::sqrt(std::abs(x));
    result +=
        (20.0 * std::sin(6.0 * x * MathUtils::OnePi) +
         20.0 * std::sin(2.0 * x * MathUtils::OnePi)) *
        2.0 / 3.0;
    result +=
        (20.0 * std::sin(y * MathUtils::OnePi) +
         40.0 * std::sin(y / 3.0 * MathUtils::OnePi)) *
        2.0 / 3.0;
    result +=
        (160.0 * std::sin(y / 12.0 * MathUtils::OnePi) +
         320.0 * std::sin(y * MathUtils::OnePi / 30.0)) *
        2.0 / 3.0;
    return result;
}

double transformLongitude(double x, double y) {
    double result =
        300.0 + x + 2.0 * y + 0.1 * x * x +
        0.1 * x * y + 0.1 * std::sqrt(std::abs(x));
    result +=
        (20.0 * std::sin(6.0 * x * MathUtils::OnePi) +
         20.0 * std::sin(2.0 * x * MathUtils::OnePi)) *
        2.0 / 3.0;
    result +=
        (20.0 * std::sin(x * MathUtils::OnePi) +
         40.0 * std::sin(x / 3.0 * MathUtils::OnePi)) *
        2.0 / 3.0;
    result +=
        (150.0 * std::sin(x / 12.0 * MathUtils::OnePi) +
         300.0 * std::sin(x / 30.0 * MathUtils::OnePi)) *
        2.0 / 3.0;
    return result;
}

} // namespace

bool Gcj02CoordinateTransform::isOutsideChina(
    const Cartographic& cartographic) {
    return cartographic.longitude() < kChinaWest ||
           cartographic.longitude() > kChinaEast ||
           cartographic.latitude() < kChinaSouth ||
           cartographic.latitude() > kChinaNorth;
}

bool Gcj02CoordinateTransform::crossesChinaBounds(
    const Rectangle& rectangle) {
    if (rectangle.isEmpty()) {
        return false;
    }
    const Rectangle bounds(kChinaWest, kChinaSouth, kChinaEast, kChinaNorth);
    return bounds.intersects(rectangle) && !bounds.contains(rectangle);
}

Cartographic Gcj02CoordinateTransform::fromWgs84(
    const Cartographic& cartographic) {
    if (isOutsideChina(cartographic)) {
        return cartographic;
    }

    const double longitudeDegrees =
        MathUtils::radiansToDegrees(cartographic.longitude());
    const double latitudeDegrees =
        MathUtils::radiansToDegrees(cartographic.latitude());
    double latitudeDelta = transformLatitude(
        longitudeDegrees - 105.0,
        latitudeDegrees - 35.0);
    double longitudeDelta = transformLongitude(
        longitudeDegrees - 105.0,
        latitudeDegrees - 35.0);

    const double latitudeRadians =
        MathUtils::degreesToRadians(latitudeDegrees);
    const double sinLatitude = std::sin(latitudeRadians);
    const double magic =
        1.0 - kEccentricitySquared * sinLatitude * sinLatitude;
    const double sqrtMagic = std::sqrt(magic);
    latitudeDelta =
        latitudeDelta * 180.0 /
        ((kSemimajorAxis * (1.0 - kEccentricitySquared)) /
         (magic * sqrtMagic) * MathUtils::OnePi);
    longitudeDelta =
        longitudeDelta * 180.0 /
        (kSemimajorAxis / sqrtMagic * std::cos(latitudeRadians) *
         MathUtils::OnePi);

    return Cartographic::fromDegrees(
        longitudeDegrees + longitudeDelta,
        latitudeDegrees + latitudeDelta,
        cartographic.height());
}

Cartographic Gcj02CoordinateTransform::toWgs84(
    const Cartographic& cartographic) {
    if (isOutsideChina(cartographic)) {
        return cartographic;
    }
    Cartographic wgs = cartographic;
    for (int i = 0; i < 3; ++i) {
        const Cartographic forward = fromWgs84(wgs);
        wgs.setLongitude(wgs.longitude() -
                         (forward.longitude() - cartographic.longitude()));
        wgs.setLatitude(wgs.latitude() -
                        (forward.latitude() - cartographic.latitude()));
    }
    return wgs;
}

Rectangle Gcj02CoordinateTransform::boundRectangleFromWgs84(
    const Rectangle& rectangle) {
    if (!rectangle.crossesAntimeridian()) {
        return boundNonCrossingRectangle(rectangle);
    }

    const Rectangle eastOfAntimeridian(
        rectangle.west(),
        rectangle.south(),
        MathUtils::OnePi,
        rectangle.north());
    const Rectangle westOfAntimeridian(
        -MathUtils::OnePi,
        rectangle.south(),
        rectangle.east(),
        rectangle.north());
    const Rectangle eastBound =
        boundNonCrossingRectangle(eastOfAntimeridian);
    const Rectangle westBound =
        boundNonCrossingRectangle(westOfAntimeridian);

    return Rectangle(
        eastBound.west(),
        std::min(eastBound.south(), westBound.south()),
        westBound.east(),
        std::max(eastBound.north(), westBound.north()));
}

} // namespace earth_engine
