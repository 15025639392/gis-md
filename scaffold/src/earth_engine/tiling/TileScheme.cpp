#include "TileScheme.h"
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

class XYZWebMercatorScheme : public TileScheme {
public:
    std::string id() const override { return "XYZ-WebMercator"; }
    std::string crsProfile() const override { return "WebMercator"; }
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
            constexpr double kMaxWebMercatorLat = 1.4844222297453324; // ~85.05112878 deg
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

} // namespace earth_engine
