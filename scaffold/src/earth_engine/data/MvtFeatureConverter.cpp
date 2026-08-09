#include "MvtFeatureConverter.h"

#include <cmath>

namespace earth_engine {

namespace {

constexpr double kPi = 3.14159265358979323846;

} // namespace

Cartographic mvtToCartographic(const TileKey& key, uint32_t extent,
                               const MvtPoint& p) {
    double n = static_cast<double>(1u << key.z);
    double e = static_cast<double>(extent == 0 ? 4096u : extent);
    // 全球归一化坐标(XYZ:y=0 在北)
    double gx = (key.x + p.x / e) / n;
    double gy = (key.y + p.y / e) / n;
    double lng = gx * 2.0 * kPi - kPi;
    double lat = 2.0 * std::atan(std::exp(kPi * (1.0 - 2.0 * gy))) - kPi / 2.0;
    return Cartographic(lng, lat, 0.0);
}

Rectangle mvtTileRectangle(const TileKey& key) {
    // 瓦片四角 = 瓦片内归一化坐标 (0,0)-(1,1);extent 取多少都一样,故传 1
    // 让 p 直接是归一化量。y=0 在北 → 北边界来自 p.y=0。
    const Cartographic nw = mvtToCartographic(key, 1u, MvtPoint{0, 0});
    const Cartographic se = mvtToCartographic(key, 1u, MvtPoint{1, 1});
    return Rectangle(nw.longitude(), se.latitude(),
                     se.longitude(), nw.latitude());
}

std::vector<Feature> mvtLayerToFeatures(const MvtLayer& layer,
                                        const TileKey& key) {
    std::vector<Feature> out;

    auto toRing = [&](const std::vector<MvtPoint>& path) {
        std::vector<Cartographic> ring;
        ring.reserve(path.size());
        for (const MvtPoint& p : path) {
            ring.push_back(mvtToCartographic(key, layer.extent, p));
        }
        return ring;
    };

    auto makeFeature = [&](const MvtFeature& src, GeometryType type) {
        Feature f;
        f.id = kInvalidFeatureId;  // 目标 store 分配
        f.sourceId = src.id != 0 ? std::to_string(src.id) : std::string();
        f.type = type;
        f.properties = src.properties;
        f.properties["mvt_layer"] = layer.name;
        f.version = 1;
        return f;
    };

    for (const MvtFeature& src : layer.features) {
        switch (src.type) {
            case MvtGeomType::Point:
                for (const auto& path : src.paths) {
                    for (const MvtPoint& p : path) {
                        Feature f = makeFeature(src, GeometryType::Point);
                        f.rings = {{mvtToCartographic(key, layer.extent, p)}};
                        out.push_back(std::move(f));
                    }
                }
                break;
            case MvtGeomType::LineString:
                for (const auto& path : src.paths) {
                    if (path.size() < 2) {
                        continue;
                    }
                    Feature f = makeFeature(src, GeometryType::LineString);
                    f.rings = {toRing(path)};
                    out.push_back(std::move(f));
                }
                break;
            case MvtGeomType::Polygon: {
                for (const MvtPolygon& poly : classifyMvtRings(src.paths)) {
                    Feature f = makeFeature(src, GeometryType::Polygon);
                    f.rings.reserve(1 + poly.holes.size());
                    f.rings.push_back(toRing(poly.exterior));
                    for (const auto& hole : poly.holes) {
                        f.rings.push_back(toRing(hole));
                    }
                    out.push_back(std::move(f));
                }
                break;
            }
            case MvtGeomType::Unknown:
                break;  // spec:未知类型丢弃
        }
    }
    return out;
}

} // namespace earth_engine
