#include "CrsProfile.h"

namespace earth_engine {

// ============================================================
// Web Mercator (EPSG:3857)
// ============================================================

namespace {

class WebMercatorProfile : public CrsProfile {
public:
    std::string id() const override { return "EPSG:3857"; }
    std::string name() const override { return "WebMercator"; }
    Unit unit() const override { return Unit::Meter; }

    std::array<double, 2> xRange() const override {
        // ~±20037508.34 m
        return {{-20037508.342789244, 20037508.342789244}};
    }

    std::array<double, 2> yRange() const override {
        return {{-20037508.342789244, 20037508.342789244}};
    }
};

// ============================================================
// WGS84 Geographic (EPSG:4326)
// ============================================================

class WGS84GeographicProfile : public CrsProfile {
public:
    std::string id() const override { return "EPSG:4326"; }
    std::string name() const override { return "WGS84Geographic"; }
    Unit unit() const override { return Unit::Degree; }

    std::array<double, 2> xRange() const override {
        return {{-180.0, 180.0}};
    }

    std::array<double, 2> yRange() const override {
        return {{-90.0, 90.0}};
    }
};

} // anonymous namespace

const CrsProfile& CrsProfile::webMercator() {
    static const WebMercatorProfile instance;
    return instance;
}

const CrsProfile& CrsProfile::wgs84Geographic() {
    static const WGS84GeographicProfile instance;
    return instance;
}

} // namespace earth_engine
