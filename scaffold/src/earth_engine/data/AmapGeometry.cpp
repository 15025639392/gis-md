#include "AmapGeometry.h"

#include "../core/geodesy/Gcj02CoordinateTransform.h"

#include <cmath>

namespace earth_engine {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;

}  // namespace

double amapCoordScale(int layerType, int layerZ) {
    if (layerType == 3) return 8192.0 / 131072.0;  // 建筑 1/16
    if (layerType == 2) return 4.0;                // 区域恒 2048×1024
    if (layerZ >= 14) return 2.0;                  // 线/轨道 z14+ 4096×2048
    if (layerZ >= 6) return 4.0;
    return 8.0;
}

Cartographic amapTileLocalToLngLat(int tileX, int tileY, int z,
                                   double localX, double localY,
                                   bool flipY) {
    const double n = std::exp2(z);
    const double lonDeg = (tileX + localX / 8192.0) / n * 360.0 - 180.0;
    const double latDeg =
        flipY ? 90.0 - (tileY + localY / 4096.0) / n * 180.0
              : (tileY + localY / 4096.0) / n * 180.0 - 90.0;
    return Cartographic(lonDeg * kDegToRad, latDeg * kDegToRad, 0.0);
}

std::vector<Feature> amapDecodedPartToFeatures(
    const AmapDecodedLayerPart& part, bool toWgs84) {
    std::vector<Feature> out;
    const double scale = amapCoordScale(part.type, part.z);
    const bool isLine = part.type == 1 || part.type == 4;
    for (const auto& f : part.features) {
        for (const auto& ring : f.rings) {
            Feature feat;
            feat.type =
                isLine ? GeometryType::LineString : GeometryType::Polygon;
            std::vector<Cartographic> pts;
            pts.reserve(ring.size());
            for (const auto& pt : ring) {
                Cartographic c =
                    amapTileLocalToLngLat(part.x, part.y, part.z,
                                          pt.first * scale, pt.second * scale);
                if (toWgs84) c = Gcj02CoordinateTransform::toWgs84(c);
                pts.push_back(c);
            }
            feat.rings.push_back(std::move(pts));
            feat.properties["amap_class"] = std::to_string(f.classCode);
            if (f.kind > 0) {
                feat.properties["amap_kind"] = std::to_string(f.kind);
            }
            if (part.type == 3 && f.height > 0.0) {
                feat.properties["amap_height"] = std::to_string(f.height);
            }
            out.push_back(std::move(feat));
        }
    }
    return out;
}

}  // namespace earth_engine
