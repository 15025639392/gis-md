#include "TileBoundingVolume.h"

#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Transforms.h"
#include "TileBoundsMetrics.h"

#include <algorithm>
#include <array>

namespace earth_engine {

namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;

void expandRectangleToCartographic(const Cartographic& cartographic,
                                   double& west,
                                   double& south,
                                   double& east,
                                   double& north) {
    west = std::min(west, cartographic.longitude());
    south = std::min(south, cartographic.latitude());
    east = std::max(east, cartographic.longitude());
    north = std::max(north, cartographic.latitude());
}

} // namespace

std::optional<OrientedBoundingBox>
TileBoundingVolume::toOrientedBoundingBox() const {
    switch (kind) {
        case TileBoundingVolumeKind::Sphere:
            return OrientedBoundingBox::fromSphere(sphere);
        case TileBoundingVolumeKind::Box:
            return box;
        case TileBoundingVolumeKind::CylinderRegion:
            return cylinderRegion.toOrientedBoundingBox();
        case TileBoundingVolumeKind::Region:
            return TileBoundsMetrics::boundingRegionObb(
                region,
                minimumHeight,
                maximumHeight);
        case TileBoundingVolumeKind::S2Cell:
            return std::nullopt;
    }
    return std::nullopt;
}

std::optional<Rectangle> TileBoundingVolume::estimateGlobeRectangle() const {
    return estimateGlobeRectangle(Ellipsoid::WGS84());
}

std::optional<Rectangle> TileBoundingVolume::estimateGlobeRectangle(
    const Ellipsoid& ellipsoid) const {
    switch (kind) {
        case TileBoundingVolumeKind::Region:
            return region;
        case TileBoundingVolumeKind::Sphere: {
            if (sphere.contains(Vec3::zero())) {
                return Rectangle::MAXIMUM;
            }

            const Mat4 enuToEcef =
                Transforms::eastNorthUpToFixedFrame(
                    sphere.getCenter(),
                    ellipsoid);
            const double radius = sphere.getRadius();
            const std::array<Vec3, 4> points = {
                enuToEcef * Vec3(radius, 0.0, 0.0),
                enuToEcef * Vec3(-radius, 0.0, 0.0),
                enuToEcef * Vec3(0.0, radius, 0.0),
                enuToEcef * Vec3(0.0, -radius, 0.0)};

            std::array<Cartographic, 4> cartographics;
            for (std::size_t i = 0; i < points.size(); ++i) {
                const std::optional<Cartographic> cartographic =
                    ellipsoid.tryCartesianToCartographic(points[i]);
                if (!cartographic) {
                    return std::nullopt;
                }
                cartographics[i] = *cartographic;
            }

            return Rectangle(
                cartographics[1].longitude(),
                cartographics[3].latitude(),
                cartographics[0].longitude(),
                cartographics[2].latitude());
        }
        case TileBoundingVolumeKind::Box: {
            if (box.contains(Vec3::zero())) {
                return Rectangle::MAXIMUM;
            }

            const Vec3& center = box.getCenter();
            const Vec3& axis0 = box.getHalfAxis(0);
            const Vec3& axis1 = box.getHalfAxis(1);
            const Vec3& axis2 = box.getHalfAxis(2);
            const std::array<Vec3, 8> corners = {
                center + axis0 + axis1 + axis2,
                center + axis0 + axis1 - axis2,
                center + axis0 - axis1 + axis2,
                center + axis0 - axis1 - axis2,
                center - axis0 + axis1 + axis2,
                center - axis0 + axis1 - axis2,
                center - axis0 - axis1 + axis2,
                center - axis0 - axis1 - axis2};

            double west = kPi;
            double south = kPi * 0.5;
            double east = -kPi;
            double north = -kPi * 0.5;
            for (const Vec3& corner : corners) {
                const std::optional<Cartographic> cartographic =
                    ellipsoid.tryCartesianToCartographic(corner);
                if (!cartographic) {
                    return std::nullopt;
                }
                expandRectangleToCartographic(
                    *cartographic,
                    west,
                    south,
                    east,
                    north);
            }

            return Rectangle(west, south, east, north);
        }
        case TileBoundingVolumeKind::CylinderRegion: {
            const OrientedBoundingBox cylinderBox =
                cylinderRegion.toOrientedBoundingBox();
            return TileBoundingVolume::fromBox(
                       cylinderBox.getCenter(),
                       cylinderBox.getHalfAxis(0),
                       cylinderBox.getHalfAxis(1),
                       cylinderBox.getHalfAxis(2))
                .estimateGlobeRectangle(ellipsoid);
        }
        case TileBoundingVolumeKind::S2Cell:
            return s2Cell.getCellID().computeBoundingRectangle();
    }
    return std::nullopt;
}

} // namespace earth_engine
