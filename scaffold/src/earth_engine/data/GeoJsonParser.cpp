#include "GeoJsonParser.h"
#include "../core/geodesy/Transforms.h"

#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <limits>

using json = nlohmann::json;

namespace earth_engine {
namespace {

/// 从 degree 坐标数组构造 Cartographic（radian）
Cartographic makeCartographic(double lngDeg, double latDeg, double heightM = 0.0) {
    return Cartographic::fromRadians(
        Transforms::toRadians(lngDeg),
        Transforms::toRadians(latDeg),
        heightM);
}

Rectangle computeBounds(const GeoFeature& feature) {
    if (feature.rings.empty() || feature.rings[0].empty()) {
        return Rectangle();
    }
    double west = std::numeric_limits<double>::max();
    double east = std::numeric_limits<double>::lowest();
    double south = std::numeric_limits<double>::max();
    double north = std::numeric_limits<double>::lowest();
    for (const auto& ring : feature.rings) {
        for (const auto& c : ring) {
            west = std::min(west, c.longitude());
            east = std::max(east, c.longitude());
            south = std::min(south, c.latitude());
            north = std::max(north, c.latitude());
        }
    }
    return Rectangle(west, south, east, north);
}

std::vector<Cartographic> parseRing(const json& coords) {
    std::vector<Cartographic> ring;
    if (!coords.is_array()) return ring;
    for (const auto& pt : coords) {
        if (!pt.is_array() || pt.size() < 2) continue;
        double lng = pt[0].get<double>();
        double lat = pt[1].get<double>();
        double h = (pt.size() >= 3) ? pt[2].get<double>() : 0.0;
        ring.push_back(makeCartographic(lng, lat, h));
    }
    return ring;
}

/// 解析单个几何体，返回若干 GeoFeature（Multi* 会展开为多个）。
/// @param geometry 几何 JSON
/// @param baseId   feature 基础 ID（Multi* 展开时追加 "-0", "-1" 等后缀）
/// @param props    属性表（所有展开特征共享）
std::vector<GeoFeature> parseGeometry(const json& geometry,
                                       const std::string& baseId,
                                       const std::unordered_map<std::string, std::string>& props) {
    std::string type = geometry.value("type", "");
    const auto& coords = geometry.value("coordinates", json::array());

    auto makeFeature = [&](GeoFeature::Type t, std::vector<std::vector<Cartographic>> r) {
        GeoFeature f;
        f.id = baseId;
        f.type = t;
        f.rings = std::move(r);
        f.properties = props;
        f.bounds = computeBounds(f);
        return f;
    };

    if (type == "Point") {
        if (coords.is_array() && coords.size() >= 2) {
            double lng = coords[0].get<double>();
            double lat = coords[1].get<double>();
            double h = (coords.size() >= 3) ? coords[2].get<double>() : 0.0;
            return {makeFeature(GeoFeature::Type::Point, {{makeCartographic(lng, lat, h)}})};
        }
    } else if (type == "LineString") {
        auto ring = parseRing(coords);
        if (!ring.empty())
            return {makeFeature(GeoFeature::Type::LineString, {std::move(ring)})};
    } else if (type == "Polygon") {
        std::vector<std::vector<Cartographic>> rings;
        if (coords.is_array()) {
            for (const auto& ring : coords)
                rings.push_back(parseRing(ring));
        }
        if (!rings.empty())
            return {makeFeature(GeoFeature::Type::Polygon, std::move(rings))};
    } else if (type == "MultiPoint") {
        std::vector<GeoFeature> results;
        if (coords.is_array()) {
            for (size_t i = 0; i < coords.size(); ++i) {
                const auto& pt = coords[i];
                if (!pt.is_array() || pt.size() < 2) continue;
                GeoFeature f;
                f.id = baseId + "-" + std::to_string(i);
                f.type = GeoFeature::Type::Point;
                double lng = pt[0].get<double>();
                double lat = pt[1].get<double>();
                double h = (pt.size() >= 3) ? pt[2].get<double>() : 0.0;
                f.rings = {{makeCartographic(lng, lat, h)}};
                f.properties = props;
                f.bounds = computeBounds(f);
                results.push_back(std::move(f));
            }
        }
        return results;
    } else if (type == "MultiLineString") {
        std::vector<GeoFeature> results;
        if (coords.is_array()) {
            for (size_t i = 0; i < coords.size(); ++i) {
                auto ring = parseRing(coords[i]);
                if (ring.empty()) continue;
                GeoFeature f;
                f.id = baseId + "-" + std::to_string(i);
                f.type = GeoFeature::Type::LineString;
                f.rings = {std::move(ring)};
                f.properties = props;
                f.bounds = computeBounds(f);
                results.push_back(std::move(f));
            }
        }
        return results;
    } else if (type == "MultiPolygon") {
        std::vector<GeoFeature> results;
        if (coords.is_array()) {
            for (size_t i = 0; i < coords.size(); ++i) {
                const auto& poly = coords[i];
                if (!poly.is_array()) continue;
                std::vector<std::vector<Cartographic>> rings;
                for (const auto& ring : poly)
                    rings.push_back(parseRing(ring));
                if (rings.empty()) continue;
                GeoFeature f;
                f.id = baseId + "-" + std::to_string(i);
                f.type = GeoFeature::Type::Polygon;
                f.rings = std::move(rings);
                f.properties = props;
                f.bounds = computeBounds(f);
                results.push_back(std::move(f));
            }
        }
        return results;
    } else if (type == "GeometryCollection") {
        std::vector<GeoFeature> results;
        if (geometry.contains("geometries") && geometry["geometries"].is_array()) {
            for (size_t i = 0; i < geometry["geometries"].size(); ++i) {
                auto children = parseGeometry(geometry["geometries"][i],
                                               baseId + "-gc" + std::to_string(i), props);
                for (auto& c : children)
                    results.push_back(std::move(c));
            }
        }
        return results;
    }

    return {};
}

std::vector<GeoFeature> parseFeature(const json& feature, int& idCounter) {
    std::string baseId;
    if (feature.contains("id")) {
        if (feature["id"].is_string()) {
            baseId = feature["id"].get<std::string>();
        } else if (feature["id"].is_number()) {
            baseId = std::to_string(feature["id"].get<int>());
        }
    } else {
        baseId = "feature-" + std::to_string(idCounter++);
    }

    std::unordered_map<std::string, std::string> props;
    if (feature.contains("properties") && feature["properties"].is_object()) {
        for (auto& [key, val] : feature["properties"].items()) {
            if (val.is_string()) {
                props[key] = val.get<std::string>();
            } else if (val.is_number()) {
                props[key] = std::to_string(val.get<double>());
            } else if (val.is_boolean()) {
                props[key] = val.get<bool>() ? "true" : "false";
            }
        }
    }

    return parseGeometry(feature["geometry"], baseId, props);
}

std::vector<GeoFeature> parseFeatureCollection(const json& fc) {
    std::vector<GeoFeature> features;
    if (!fc.contains("type") || fc["type"] != "FeatureCollection") return features;
    if (!fc.contains("features") || !fc["features"].is_array()) return features;
    int idCounter = 0;
    for (const auto& f : fc["features"]) {
        if (!f.contains("type") || f["type"] != "Feature") continue;
        if (!f.contains("geometry") || f["geometry"].is_null()) continue;
        auto parsedFeatures = parseFeature(f, idCounter);
        for (auto& feat : parsedFeatures) {
            if (!feat.rings.empty()) {
                features.push_back(std::move(feat));
            }
        }
    }
    return features;
}

} // anonymous namespace

// ============================================================
// GeoJsonParser
// ============================================================

std::vector<GeoFeature> GeoJsonParser::parse(const std::string& jsonText) {
    try {
        json doc = json::parse(jsonText);
        return parseFeatureCollection(doc);
    } catch (const json::parse_error&) {
        return {};
    } catch (const json::type_error&) {
        return {};
    }
}

} // namespace earth_engine
