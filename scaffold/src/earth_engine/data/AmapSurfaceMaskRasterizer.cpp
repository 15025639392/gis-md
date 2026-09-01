#include "AmapSurfaceMaskRasterizer.h"
#include "../core/geodesy/WebMercatorProjection.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace earth_engine {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kTwoPi = 2.0 * kPi;

struct Point {
    double x = 0.0;
    double y = 0.0;
};

bool samePoint(const Point& a, const Point& b) {
    return a.x == b.x && a.y == b.y;
}

void removeDegenerateVertices(std::vector<Point>& ring) {
    if (ring.empty()) return;
    std::vector<Point> compact;
    compact.reserve(ring.size());
    for (const Point& p : ring) {
        if (compact.empty() || !samePoint(compact.back(), p)) {
            compact.push_back(p);
        }
    }
    if (compact.size() > 1 && samePoint(compact.front(), compact.back())) {
        compact.pop_back();
    }
    ring.swap(compact);
}

double normalizedLongitude(double longitude, const Rectangle& bounds) {
    double west = bounds.west();
    double east = bounds.east();
    if (east < west) {
        east += kTwoPi;
        // Match Rectangle::normalizedCoordinates for anti-meridian pages:
        // west-side longitudes refer to the page's eastern wrapped copy.
        if (longitude < west) longitude += kTwoPi;
    } else {
        // A Feature can originate from a wrapped vector tile.  Bring
        // longitude to the nearest equivalent of this page in O(1), rather
        // than creating an artificial edge across the globe.
        const double center = (west + east) * 0.5;
        longitude = center + std::remainder(longitude - center, kTwoPi);
    }
    return (longitude - west) / (east - west);
}

Point toTileUnit(
    const Cartographic& c, const Rectangle& bounds,
    AmapSurfaceMaskRasterizerOptions::Projection projection) {
    const double width = bounds.width();
    const double height = bounds.north() - bounds.south();
    if (!(width > 0.0) || !(height > 0.0)) return {};
    double y = c.latitude();
    double north = bounds.north();
    double south = bounds.south();
    if (projection == AmapSurfaceMaskRasterizerOptions::Projection::WebMercator) {
        y = WebMercatorProjection::geodeticLatitudeToMercatorAngle(y);
        north = WebMercatorProjection::geodeticLatitudeToMercatorAngle(north);
        south = WebMercatorProjection::geodeticLatitudeToMercatorAngle(south);
    }
    const double projectedHeight = north - south;
    if (!(projectedHeight > 0.0)) return {};
    return {normalizedLongitude(c.longitude(), bounds),
            (north - y) / projectedHeight};
}

void rasterizeEvenOdd(const std::vector<std::vector<Point>>& rings, int hi,
                      std::vector<uint8_t>& destination) {
    if (rings.empty() || hi <= 0) return;

    struct Edge {
        Point a;
        Point b;
    };
    struct Hit {
        double x;
    };
    std::vector<Edge> edges;
    double minY = std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    for (const auto& ring : rings) {
        edges.reserve(edges.size() + ring.size());
        for (size_t i = 0; i < ring.size(); ++i) {
            const Point& a = ring[i];
            const Point& b = ring[(i + 1) % ring.size()];
            if (a.y == b.y) continue;
            edges.push_back({a, b});
            minY = std::min(minY, std::min(a.y, b.y));
            maxY = std::max(maxY, std::max(a.y, b.y));
        }
    }
    if (edges.empty()) return;

    std::vector<Hit> hits;
    hits.reserve(64);
    const double rasterMin = -0.5;
    const double rasterMax = static_cast<double>(hi) + 0.5;
    const double clippedMinY = std::clamp(minY, rasterMin, rasterMax);
    const double clippedMaxY = std::clamp(maxY, rasterMin, rasterMax);
    const int yBegin = std::max(
        0, static_cast<int>(std::ceil(clippedMinY - 0.5)));
    const int yEnd = std::min(
        hi, static_cast<int>(std::ceil(clippedMaxY - 0.5)));
    for (int y = yBegin; y < yEnd; ++y) {
        const double sampleY = static_cast<double>(y) + 0.5;
        hits.clear();
        for (const Edge& edge : edges) {
            const Point& a = edge.a;
            const Point& b = edge.b;
            const double y0 = std::min(a.y, b.y);
            const double y1 = std::max(a.y, b.y);
            // Half-open top/bottom convention avoids counting a vertex twice
            // when two edges meet exactly on a scanline.
            if (sampleY < y0 || sampleY >= y1) continue;
            const double t = (sampleY - a.y) / (b.y - a.y);
            hits.push_back({a.x + t * (b.x - a.x)});
        }
        if (hits.size() < 2) continue;
        std::sort(hits.begin(), hits.end(),
                  [](const Hit& lhs, const Hit& rhs) { return lhs.x < rhs.x; });

        uint8_t* row = destination.data() + static_cast<size_t>(y) * hi;
        for (size_t i = 0; i + 1 < hits.size(); i += 2) {
            double left = hits[i].x;
            double right = hits[i + 1].x;
            if (right < left) std::swap(left, right);
            if (right <= rasterMin || left >= rasterMax) continue;
            left = std::clamp(left, rasterMin, rasterMax);
            right = std::clamp(right, rasterMin, rasterMax);
            // A pixel center x+0.5 is covered iff left <= x+0.5 < right.
            const int first = std::max(0, static_cast<int>(std::ceil(left - 0.5)));
            const int last = std::min(
                hi - 1, static_cast<int>(std::ceil(right - 0.5)) - 1);
            for (int x = first; x <= last; ++x) row[x] = 1;
        }
    }
}

AmapSurfaceMask rasterizeImpl(const std::vector<const Feature*>& features,
                               const Rectangle& tileBounds,
                               const AmapSurfaceMaskRasterizerOptions& options) {
    AmapSurfaceMask out;
    if (options.size <= 0 || tileBounds.isEmpty() ||
        !(tileBounds.width() > 0.0) || !(tileBounds.north() > tileBounds.south())) {
        return out;
    }
    const int size = options.size;
    const int supersample = std::clamp(options.supersample, 2, 4);
    if (size > std::numeric_limits<int>::max() / supersample) return out;
    const int hi = size * supersample;
    std::vector<uint8_t> highResolution(static_cast<size_t>(hi) * hi, 0);

    for (const Feature* feature : features) {
        if (feature == nullptr || feature->type != GeometryType::Polygon) continue;
        std::vector<std::vector<Point>> tileLocalRings;
        tileLocalRings.reserve(feature->rings.size());
        for (const auto& sourceRing : feature->rings) {
            std::vector<Point> unitRing;
            unitRing.reserve(sourceRing.size());
            bool valid = true;
            for (const Cartographic& c : sourceRing) {
                if (!std::isfinite(c.longitude()) ||
                    !std::isfinite(c.latitude())) {
                    valid = false;
                    break;
                }
                const Point p = toTileUnit(c, tileBounds, options.projection);
                if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
                    valid = false;
                    break;
                }
                unitRing.push_back(p);
            }
            if (!valid) continue;
            removeDegenerateVertices(unitRing);
            if (unitRing.size() < 3) continue;
            for (Point& p : unitRing) {
                p.x *= hi;
                p.y *= hi;
            }
            tileLocalRings.push_back(std::move(unitRing));
        }
        // Do not Sutherland-Hodgman each concave ring here.  Its intersection
        // with a rectangle can split into multiple components, which a single
        // output ring cannot represent without a fake bridge.  The scanline
        // bounds below are an exact raster-space clip and preserve topology.
        // rasterizeEvenOdd only ever writes 1, so drawing each Feature into
        // the shared destination implements union without a full-page scratch
        // clear/copy per source record.
        rasterizeEvenOdd(tileLocalRings, hi, highResolution);
    }

    out.size = size;
    out.coverage.assign(static_cast<size_t>(size) * size, 0);
    const int sampleCount = supersample * supersample;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int covered = 0;
            for (int sy = 0; sy < supersample; ++sy) {
                const uint8_t* row = highResolution.data() +
                    static_cast<size_t>(y * supersample + sy) * hi;
                for (int sx = 0; sx < supersample; ++sx) {
                    covered += row[x * supersample + sx] != 0;
                }
            }
            out.coverage[static_cast<size_t>(y) * size + x] =
                static_cast<uint8_t>(std::lround(
                    static_cast<double>(covered) * 255.0 / sampleCount));
        }
    }
    return out;
}

}  // namespace

AmapSurfaceMask rasterizeAmapSurfaceMask(
    const std::vector<const Feature*>& features, const Rectangle& tileBounds,
    const AmapSurfaceMaskRasterizerOptions& options) {
    return rasterizeImpl(features, tileBounds, options);
}

AmapSurfaceMask rasterizeAmapSurfaceMask(
    const Feature& feature, const Rectangle& tileBounds,
    const AmapSurfaceMaskRasterizerOptions& options) {
    const std::vector<const Feature*> features{&feature};
    return rasterizeImpl(features, tileBounds, options);
}

Rectangle amapSurfaceMaskTileRectangle(const TileKey& key) {
    if (key.z < 0 || key.z > 30) return Rectangle::EMPTY;
    const int tileCount = 1 << key.z;
    if (key.x < 0 || key.y < 0 || key.x >= tileCount || key.y >= tileCount) {
        return Rectangle::EMPTY;
    }
    const double n = static_cast<double>(tileCount);
    const double west = static_cast<double>(key.x) / n * 360.0 - 180.0;
    const double east = static_cast<double>(key.x + 1) / n * 360.0 - 180.0;
    const double north = 90.0 - static_cast<double>(key.y) / n * 180.0;
    const double south = 90.0 - static_cast<double>(key.y + 1) / n * 180.0;
    constexpr double kDegToRad = kPi / 180.0;
    return Rectangle(west * kDegToRad, south * kDegToRad, east * kDegToRad,
                     north * kDegToRad);
}

AmapSurfaceMask rasterizeAmapSurfaceMask(
    const std::vector<const Feature*>& features, const TileKey& key,
    const AmapSurfaceMaskRasterizerOptions& options) {
    return rasterizeImpl(features, amapSurfaceMaskTileRectangle(key), options);
}

AmapSurfaceMask rasterizeAmapSurfaceMask(
    const std::vector<Feature>& features, const Rectangle& tileBounds,
    const AmapSurfaceMaskRasterizerOptions& options) {
    std::vector<const Feature*> views;
    views.reserve(features.size());
    for (const Feature& feature : features) views.push_back(&feature);
    return rasterizeImpl(views, tileBounds, options);
}

AmapSurfaceMask rasterizeAmapSurfaceMask(
    const std::vector<Feature>& features, const TileKey& key,
    const AmapSurfaceMaskRasterizerOptions& options) {
    std::vector<const Feature*> views;
    views.reserve(features.size());
    for (const Feature& feature : features) views.push_back(&feature);
    return rasterizeImpl(views, amapSurfaceMaskTileRectangle(key), options);
}

}  // namespace earth_engine
