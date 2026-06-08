#include "TileScheme.h"
#include "CrsProfile.h"
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
constexpr double kMaxWebMercatorLat = 1.4844222297453324;

enum class OpenGlobusTileGroup {
    Mercator = 0,
    NorthPolar = 1,
    SouthPolar = 2
};

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
    int maxZoom() const override { return 20; }
    std::string yDirection() const override { return "down"; }

    Rectangle tileToRectangle(const TileKey& key) const override {
        int z = key.z, x = key.x, y = key.y;
        int tilesAtZoom = 1 << z;

        double xMin = static_cast<double>(x) / tilesAtZoom;
        double xMax = static_cast<double>(x + 1) / tilesAtZoom;
        double yMin = static_cast<double>(y) / tilesAtZoom;
        double yMax = static_cast<double>(y + 1) / tilesAtZoom;

        auto mercatorToLng = [](double mx) -> double {
            return mx * glm::two_pi<double>() - glm::pi<double>();
        };
        auto mercatorToLat = [](double my) -> double {
            double latRad = glm::pi<double>() - glm::two_pi<double>() * my;
            return std::atan(std::sinh(latRad));
        };

        // XYZ y 轴：北（y=0）→ 南（yMax=1）
        return Rectangle(mercatorToLng(xMin), mercatorToLat(yMax),
                         mercatorToLng(xMax), mercatorToLat(yMin));
    }

    TileKey positionToTile(double lngRad, double latRad, int zoom) const override {
        int tilesAtZoom = 1 << zoom;

        auto lngToMercator = [](double lng) -> double {
            return (lng + glm::pi<double>()) / glm::two_pi<double>();
        };
        auto latToMercator = [](double lat) -> double {
            double clampedLat = std::clamp(lat, -kMaxWebMercatorLat, kMaxWebMercatorLat);
            return (1.0 - std::log(std::tan(clampedLat * 0.5 + glm::pi<double>() / 4.0)) /
                          glm::pi<double>()) * 0.5;
        };

        double mx = lngToMercator(lngRad);
        double my = latToMercator(latRad);

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
    int maxZoom() const override { return 20; }
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

        auto mercatorToLng = [](double mx) -> double {
            return mx * glm::two_pi<double>() - glm::pi<double>();
        };
        auto mercatorToLat = [](double my) -> double {
            double latRad = glm::pi<double>() - glm::two_pi<double>() * my;
            return std::atan(std::sinh(latRad));
        };

        return Rectangle(mercatorToLng(xMin), mercatorToLat(mySouth),
                         mercatorToLng(xMax), mercatorToLat(myNorth));
    }

    TileKey positionToTile(double lngRad, double latRad, int zoom) const override {
        int tilesAtZoom = 1 << zoom;

        auto lngToMercator = [](double lng) -> double {
            return (lng + glm::pi<double>()) / glm::two_pi<double>();
        };
        auto latToMercator = [](double lat) -> double {
            double clampedLat = std::clamp(lat, -kMaxWebMercatorLat, kMaxWebMercatorLat);
            return (1.0 - std::log(std::tan(clampedLat * 0.5 + glm::pi<double>() / 4.0)) /
                          glm::pi<double>()) * 0.5;
        };

        double mx = lngToMercator(lngRad);
        double my = latToMercator(latRad);  // 0=北, 1=南

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
    int maxZoom() const override { return 20; }
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
        auto mercatorToLng = [](double mx) -> double {
            return mx * glm::two_pi<double>() - glm::pi<double>();
        };
        auto mercatorToLat = [](double my) -> double {
            double latRad = glm::pi<double>() - glm::two_pi<double>() * my;
            return std::atan(std::sinh(latRad));
        };
        return Rectangle(mercatorToLng(xMin), mercatorToLat(yMax),
                         mercatorToLng(xMax), mercatorToLat(yMin));
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
            const double clampedLat = std::clamp(latRad, -kMaxWebMercatorLat, kMaxWebMercatorLat);
            const double my = (1.0 - std::log(std::tan(clampedLat * 0.5 +
                glm::pi<double>() / 4.0)) / glm::pi<double>()) * 0.5;
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

} // namespace earth_engine
