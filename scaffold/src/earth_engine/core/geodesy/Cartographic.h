#pragma once

#include <ostream>

namespace earth_engine {

/// 大地坐标（经度、纬度、高度）。
/// 内部存储单位：longitude/latitude = radian，height = meter。
/// 构造方法区分 degree 和 radian。
class Cartographic {
public:
    Cartographic() : lng_(0), lat_(0), height_(0) {}
    Cartographic(double lngRad, double latRad, double heightM = 0.0)
        : lng_(lngRad), lat_(latRad), height_(heightM) {}

    /// 从弧度构造
    static Cartographic fromRadians(double lngRad, double latRad, double heightM = 0) {
        return Cartographic(lngRad, latRad, heightM);
    }

    /// 从度构造
    static Cartographic fromDegrees(double lngDeg, double latDeg, double heightM = 0);

    double longitude() const { return lng_; }
    double latitude() const { return lat_; }
    double height() const { return height_; }

    double longitudeDegrees() const;
    double latitudeDegrees() const;

    void setLongitude(double rad) { lng_ = rad; }
    void setLatitude(double rad) { lat_ = rad; }
    void setHeight(double m) { height_ = m; }

    bool operator==(const Cartographic& rhs) const;
    bool operator!=(const Cartographic& rhs) const;

private:
    double lng_;     // radian
    double lat_;     // radian
    double height_;  // meter
};

std::ostream& operator<<(std::ostream& os, const Cartographic& c);

} // namespace earth_engine
