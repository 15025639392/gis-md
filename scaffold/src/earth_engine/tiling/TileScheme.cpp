#include "TileScheme.h"
#include "CrsProfile.h"
#include "../core/geodesy/WebMercatorProjection.h"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace earth_engine {

// ============================================================
// XYZ Web Mercator 实现
// ============================================================

namespace {
const double kMaxWebMercatorLat = WebMercatorProjection::maximumLatitude();

enum class OpenGlobusTileGroup {
    Mercator = 0,
    NorthPolar = 1,
    SouthPolar = 2
};

double mercatorFractionToLongitude(double fraction) {
    return fraction * glm::two_pi<double>() - glm::pi<double>();
}

double longitudeToMercatorFraction(double longitude) {
    return (longitude + glm::pi<double>()) / glm::two_pi<double>();
}

double mercatorFractionToLatitude(double fraction) {
    const double mercatorAngle = glm::pi<double>() -
        glm::two_pi<double>() * fraction;
    return WebMercatorProjection::mercatorAngleToGeodeticLatitude(
        mercatorAngle);
}

double latitudeToMercatorFraction(double latitude) {
    return (1.0 -
        WebMercatorProjection::geodeticLatitudeToMercatorAngle(latitude) /
            glm::pi<double>()) * 0.5;
}

int groupBaseY(OpenGlobusTileGroup group, int tilesAtZoom) {
    return static_cast<int>(group) * tilesAtZoom;
}

OpenGlobusTileGroup groupForY(int y, int tilesAtZoom) {
    if (y >= 2 * tilesAtZoom) return OpenGlobusTileGroup::SouthPolar;
    if (y >= tilesAtZoom) return OpenGlobusTileGroup::NorthPolar;
    return OpenGlobusTileGroup::Mercator;
}

class XYZWebMercatorScheme : public TileScheme {
public:
    std::string id() const override { return "XYZ-WebMercator"; }
    const CrsProfile& crs() const override { return CrsProfile::webMercator(); }
    std::string crsProfile() const override { return crs().id(); }
    int tileSize() const override { return 256; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 22; }
    std::string yDirection() const override { return "down"; }

    Rectangle tileToRectangle(const TileKey& key) const override {
        int z = key.z, x = key.x, y = key.y;
        int tilesAtZoom = 1 << z;

        double xMin = static_cast<double>(x) / tilesAtZoom;
        double xMax = static_cast<double>(x + 1) / tilesAtZoom;
        double yMin = static_cast<double>(y) / tilesAtZoom;
        double yMax = static_cast<double>(y + 1) / tilesAtZoom;

        // XYZ y 轴：北（y=0）→ 南（yMax=1）
        return Rectangle(mercatorFractionToLongitude(xMin),
                         mercatorFractionToLatitude(yMax),
                         mercatorFractionToLongitude(xMax),
                         mercatorFractionToLatitude(yMin));
    }

    TileKey positionToTile(double lngRad, double latRad, int zoom) const override {
        int tilesAtZoom = 1 << zoom;

        double mx = longitudeToMercatorFraction(lngRad);
        double my = latitudeToMercatorFraction(latRad);

        int x = std::clamp(static_cast<int>(mx * tilesAtZoom), 0, tilesAtZoom - 1);
        int y = std::clamp(static_cast<int>(my * tilesAtZoom), 0, tilesAtZoom - 1);

        return TileKey{id(), zoom, x, y};
    }

    void tileRange(const Rectangle& rect, int zoom,
                   int& minX, int& minY, int& maxX, int& maxY) const override {
        TileKey sw = positionToTile(rect.west(), rect.south(), zoom);
        TileKey ne = positionToTile(rect.east(), rect.north(), zoom);
        minX = std::min(sw.x, ne.x);
        maxX = std::max(sw.x, ne.x);
        minY = std::min(sw.y, ne.y);
        maxY = std::max(sw.y, ne.y);
    }

    double levelResolution(int zoom) const override {
        // Web Mercator: 整个球面宽度（2π）除以该 zoom 的 tile 数
        return glm::two_pi<double>() / static_cast<double>(1 << zoom);
    }
};

} // anonymous namespace

std::unique_ptr<TileScheme> TileScheme::createXYZWebMercator() {
    return std::make_unique<XYZWebMercatorScheme>();
}

// ============================================================
// TMS Web Mercator 实现
// ============================================================

namespace {

/// TMS（Tile Map Service）Web Mercator。
/// 与 XYZ 共享 CRS，但 y 轴方向相反：
///   - XYZ: y=0 在北侧（"down"）
///   - TMS: y=0 在南侧（"up"）
/// 转换关系：y_tms = (2^z - 1) - y_xyz
class TMSWebMercatorScheme : public TileScheme {
public:
    std::string id() const override { return "TMS-WebMercator"; }
    const CrsProfile& crs() const override { return CrsProfile::webMercator(); }
    std::string crsProfile() const override { return crs().id(); }
    int tileSize() const override { return 256; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 22; }
    std::string yDirection() const override { return "up"; }

    Rectangle tileToRectangle(const TileKey& key) const override {
        int z = key.z, x = key.x, y = key.y;
        int tilesAtZoom = 1 << z;

        double xMin = static_cast<double>(x) / tilesAtZoom;
        double xMax = static_cast<double>(x + 1) / tilesAtZoom;
        // TMS: y=0 在南侧。Mercator fraction my: 0=北, 1=南。
        // my_south = 1 - y/tilesAtZoom, my_north = 1 - (y+1)/tilesAtZoom
        double myNorth = static_cast<double>(tilesAtZoom - y - 1) / tilesAtZoom;
        double mySouth = static_cast<double>(tilesAtZoom - y) / tilesAtZoom;

        return Rectangle(mercatorFractionToLongitude(xMin),
                         mercatorFractionToLatitude(mySouth),
                         mercatorFractionToLongitude(xMax),
                         mercatorFractionToLatitude(myNorth));
    }

    TileKey positionToTile(double lngRad, double latRad, int zoom) const override {
        int tilesAtZoom = 1 << zoom;

        double mx = longitudeToMercatorFraction(lngRad);
        double my = latitudeToMercatorFraction(latRad);  // 0=北, 1=南

        // TMS: y=0 在南侧 → my_tms = 1 - my
        double myTms = 1.0 - my;

        int x = std::clamp(static_cast<int>(mx * tilesAtZoom), 0, tilesAtZoom - 1);
        int y = std::clamp(static_cast<int>(myTms * tilesAtZoom), 0, tilesAtZoom - 1);

        return TileKey{id(), zoom, x, y};
    }

    void tileRange(const Rectangle& rect, int zoom,
                   int& minX, int& minY, int& maxX, int& maxY) const override {
        TileKey sw = positionToTile(rect.west(), rect.south(), zoom);
        TileKey ne = positionToTile(rect.east(), rect.north(), zoom);
        minX = std::min(sw.x, ne.x);
        maxX = std::max(sw.x, ne.x);
        // TMS: south 对应较小的 y，north 对应较大的 y
        minY = std::min(sw.y, ne.y);
        maxY = std::max(sw.y, ne.y);
    }

    double levelResolution(int zoom) const override {
        return glm::two_pi<double>() / static_cast<double>(1 << zoom);
    }
};

} // anonymous namespace

std::unique_ptr<TileScheme> TileScheme::createTMS() {
    return std::make_unique<TMSWebMercatorScheme>();
}

namespace {

class OpenGlobusEarthScheme : public TileScheme {
public:
    std::string id() const override { return "OpenGlobus-Earth"; }
    const CrsProfile& crs() const override { return CrsProfile::webMercator(); }
    std::string crsProfile() const override { return "EPSG:3857+polar-lonlat"; }
    int tileSize() const override { return 256; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 22; }
    std::string yDirection() const override { return "down-grouped"; }

    Rectangle tileToRectangle(const TileKey& key) const override {
        const int z = key.z;
        const int tilesAtZoom = 1 << z;
        const int x = std::clamp(key.x, 0, tilesAtZoom - 1);
        const OpenGlobusTileGroup group = groupForY(key.y, tilesAtZoom);
        const int localY = std::clamp(
            key.y - groupBaseY(group, tilesAtZoom),
            0,
            tilesAtZoom - 1);

        const double west = -glm::pi<double>() +
            glm::two_pi<double>() * static_cast<double>(x) / tilesAtZoom;
        const double east = -glm::pi<double>() +
            glm::two_pi<double>() * static_cast<double>(x + 1) / tilesAtZoom;

        if (group == OpenGlobusTileGroup::NorthPolar) {
            const double height = (glm::half_pi<double>() - kMaxWebMercatorLat) /
                static_cast<double>(tilesAtZoom);
            const double north = glm::half_pi<double>() - height * localY;
            const double south = glm::half_pi<double>() - height * (localY + 1);
            return Rectangle(west, south, east, north);
        }
        if (group == OpenGlobusTileGroup::SouthPolar) {
            const double height = (glm::half_pi<double>() - kMaxWebMercatorLat) /
                static_cast<double>(tilesAtZoom);
            const double south = -glm::half_pi<double>() + height * localY;
            const double north = -glm::half_pi<double>() + height * (localY + 1);
            return Rectangle(west, south, east, north);
        }

        const double xMin = static_cast<double>(x) / tilesAtZoom;
        const double xMax = static_cast<double>(x + 1) / tilesAtZoom;
        const double yMin = static_cast<double>(localY) / tilesAtZoom;
        const double yMax = static_cast<double>(localY + 1) / tilesAtZoom;
        return Rectangle(mercatorFractionToLongitude(xMin),
                         mercatorFractionToLatitude(yMax),
                         mercatorFractionToLongitude(xMax),
                         mercatorFractionToLatitude(yMin));
    }

    TileKey positionToTile(double lngRad, double latRad, int zoom) const override {
        const int tilesAtZoom = 1 << zoom;
        const double lngNorm = std::clamp(
            (lngRad + glm::pi<double>()) / glm::two_pi<double>(),
            0.0,
            std::nextafter(1.0, 0.0));
        const int x = std::clamp(static_cast<int>(lngNorm * tilesAtZoom),
                                 0,
                                 tilesAtZoom - 1);

        OpenGlobusTileGroup group = OpenGlobusTileGroup::Mercator;
        int localY = 0;
        if (latRad > kMaxWebMercatorLat) {
            group = OpenGlobusTileGroup::NorthPolar;
            const double t = std::clamp(
                (glm::half_pi<double>() - latRad) /
                    (glm::half_pi<double>() - kMaxWebMercatorLat),
                0.0,
                std::nextafter(1.0, 0.0));
            localY = static_cast<int>(t * tilesAtZoom);
        } else if (latRad < -kMaxWebMercatorLat) {
            group = OpenGlobusTileGroup::SouthPolar;
            const double t = std::clamp(
                (latRad + glm::half_pi<double>()) /
                    (glm::half_pi<double>() - kMaxWebMercatorLat),
                0.0,
                std::nextafter(1.0, 0.0));
            localY = static_cast<int>(t * tilesAtZoom);
        } else {
            const double my = latitudeToMercatorFraction(latRad);
            localY = static_cast<int>(std::clamp(my, 0.0, std::nextafter(1.0, 0.0)) *
                                      tilesAtZoom);
        }

        localY = std::clamp(localY, 0, tilesAtZoom - 1);
        return TileKey{id(), zoom, x, groupBaseY(group, tilesAtZoom) + localY};
    }

    void tileRange(const Rectangle& rect, int zoom,
                   int& minX, int& minY, int& maxX, int& maxY) const override {
        TileKey sw = positionToTile(rect.west(), rect.south(), zoom);
        TileKey ne = positionToTile(rect.east(), rect.north(), zoom);
        minX = std::min(sw.x, ne.x);
        maxX = std::max(sw.x, ne.x);
        minY = std::min(sw.y, ne.y);
        maxY = std::max(sw.y, ne.y);
    }

    double levelResolution(int zoom) const override {
        return glm::two_pi<double>() / static_cast<double>(1 << zoom);
    }
};

} // anonymous namespace

std::unique_ptr<TileScheme> TileScheme::createOpenGlobusEarth() {
    return std::make_unique<OpenGlobusEarthScheme>();
}

namespace {

class GeographicTMSScheme : public TileScheme {
public:
    std::string id() const override { return "Geographic-TMS"; }
    const CrsProfile& crs() const override { return CrsProfile::wgs84Geographic(); }
    std::string crsProfile() const override { return "EPSG:4326"; }
    int tileSize() const override { return 256; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 22; }
    std::string yDirection() const override { return "up"; }  // y=0 = south

    Rectangle tileToRectangle(const TileKey& key) const override {
        int z = key.z, x = key.x, y = key.y;
        double xTilesAtZ = static_cast<double>(1 << (z + 1));
        double yTilesAtZ = static_cast<double>(1 << z);
        double west  = static_cast<double>(x)     / xTilesAtZ * 360.0 - 180.0;
        double east  = static_cast<double>(x + 1) / xTilesAtZ * 360.0 - 180.0;
        // y=0 at south pole, y increases northward (EPSG:4326 standard)
        double south = -90.0 + static_cast<double>(y)     / yTilesAtZ * 180.0;
        double north = -90.0 + static_cast<double>(y + 1) / yTilesAtZ * 180.0;
        return Rectangle(
            glm::radians(west), glm::radians(south),
            glm::radians(east), glm::radians(north));
    }

    TileKey positionToTile(double lngRad, double latRad, int zoom) const override {
        double lngDeg = glm::degrees(lngRad);
        double latDeg = glm::degrees(latRad);
        int xTilesAtZoom = 1 << (zoom + 1);
        int yTilesAtZoom = 1 << zoom;
        int x = static_cast<int>((lngDeg + 180.0) / 360.0 * xTilesAtZoom);
        // y=0 at south pole: y = (lat + 90) / 180 * 2^z
        int y = static_cast<int>((latDeg + 90.0) / 180.0 * yTilesAtZoom);
        x = std::clamp(x, 0, xTilesAtZoom - 1);
        y = std::clamp(y, 0, yTilesAtZoom - 1);
        return TileKey{id(), zoom, x, y};
    }

    void tileRange(const Rectangle& rect, int zoom,
                   int& minX, int& minY, int& maxX, int& maxY) const override {
        TileKey sw = positionToTile(rect.west(), rect.south(), zoom);
        TileKey ne = positionToTile(rect.east(), rect.north(), zoom);
        minX = std::min(sw.x, ne.x); maxX = std::max(sw.x, ne.x);
        minY = std::min(sw.y, ne.y); maxY = std::max(sw.y, ne.y);
    }

    double levelResolution(int zoom) const override {
        return glm::radians(180.0) / static_cast<double>(1 << zoom);
    }
};

} // anonymous namespace

std::unique_ptr<TileScheme> TileScheme::createGeographicTMS() {
    return std::make_unique<GeographicTMSScheme>();
}

} // namespace earth_engine
