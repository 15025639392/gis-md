#pragma once

#include <optional>
#include <ostream>
#include <utility>

namespace earth_engine {

/// 经纬度矩形包围盒。
/// 单位：radian（内部）；构造时支持 degree（通过 fromDegrees）。
/// 跨反经线时 west > east。
class Rectangle {
public:
    Rectangle() : west_(0), south_(0), east_(0), north_(0) {}
    Rectangle(double west, double south, double east, double north)
        : west_(west), south_(south), east_(east), north_(north) {}

    static Rectangle fromDegrees(double westDeg, double southDeg,
                                  double eastDeg, double northDeg);

    double west() const { return west_; }
    double south() const { return south_; }
    double east() const { return east_; }
    double north() const { return north_; }

    double westDegrees() const;
    double southDegrees() const;
    double eastDegrees() const;
    double northDegrees() const;

    double width() const;   // 经度跨度 (rad)
    double height() const;  // 纬度跨度 (rad)
    std::pair<double, double> center() const;
    std::pair<double, double> normalizedCoordinates(double lngRad,
                                                    double latRad) const;

    bool contains(double lngRad, double latRad) const;
    bool contains(const Rectangle& other) const;
    bool intersects(const Rectangle& other) const;
    bool isEmpty() const;

    double computeSignedDistance(double lngRad, double latRad) const;
    std::optional<Rectangle> computeIntersection(const Rectangle& other) const;
    Rectangle computeUnion(const Rectangle& other) const;
    std::pair<Rectangle, std::optional<Rectangle>> splitAtAntimeridian() const;

    /// 跨反经线（west > east）
    bool crossesAntimeridian() const;

    bool operator==(const Rectangle& rhs) const;
    bool operator!=(const Rectangle& rhs) const;

private:
    double west_, south_, east_, north_;
};

std::ostream& operator<<(std::ostream& os, const Rectangle& r);

} // namespace earth_engine
