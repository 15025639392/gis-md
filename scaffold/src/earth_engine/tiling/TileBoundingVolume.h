#pragma once

#include "../core/math/BoundingSphere.h"
#include "../core/geodesy/S2CellBoundingVolume.h"
#include "../core/math/BoundingCylinderRegion.h"
#include "../core/math/Mat4.h"
#include "../core/math/OrientedBoundingBox.h"
#include "../core/math/Rectangle.h"

#include <optional>

namespace earth_engine {

class Ellipsoid;

enum class TileBoundingVolumeKind {
    Region,
    Sphere,
    Box,
    CylinderRegion,
    S2Cell
};

/// cesium-native BoundingVolume subset used by explicit 3D Tiles JSON.
/// Regions are stored in radians/meters and are not transformed by tile
/// transforms, matching cesium-native transformBoundingVolume.
struct TileBoundingVolume {
    TileBoundingVolumeKind kind = TileBoundingVolumeKind::Region;
    Rectangle region;
    double minimumHeight = 0.0;
    double maximumHeight = 0.0;
    bool looseFittingHeights = false;
    BoundingSphere sphere{Vec3::zero(), 0.0};
    OrientedBoundingBox box{
        Vec3::zero(),
        Vec3::zero(),
        Vec3::zero(),
        Vec3::zero()};
    BoundingCylinderRegion cylinderRegion{
        Vec3::zero(),
        glm::dquat(1.0, 0.0, 0.0, 0.0),
        0.0,
        glm::dvec2(0.0, 0.0)};
    S2CellBoundingVolume s2Cell{
        S2CellID(0),
        0.0,
        0.0};

    static TileBoundingVolume fromRegion(const Rectangle& rectangle,
                                         double minHeight,
                                         double maxHeight) {
        TileBoundingVolume volume;
        volume.kind = TileBoundingVolumeKind::Region;
        volume.region = rectangle;
        volume.minimumHeight = minHeight;
        volume.maximumHeight = maxHeight;
        return volume;
    }

    static TileBoundingVolume fromLooseRegion(const Rectangle& rectangle,
                                              double minHeight,
                                              double maxHeight) {
        TileBoundingVolume volume = fromRegion(
            rectangle,
            minHeight,
            maxHeight);
        volume.looseFittingHeights = true;
        return volume;
    }

    static TileBoundingVolume fromSphere(const Vec3& center,
                                         double radius) {
        TileBoundingVolume volume;
        volume.kind = TileBoundingVolumeKind::Sphere;
        volume.sphere = BoundingSphere(center, radius);
        return volume;
    }

    static TileBoundingVolume fromBox(const Vec3& center,
                                      const Vec3& axis0,
                                      const Vec3& axis1,
                                      const Vec3& axis2) {
        TileBoundingVolume volume;
        volume.kind = TileBoundingVolumeKind::Box;
        volume.box = OrientedBoundingBox(center, axis0, axis1, axis2);
        return volume;
    }

    static TileBoundingVolume fromCylinderRegion(
        const BoundingCylinderRegion& cylinderRegion) {
        TileBoundingVolume volume;
        volume.kind = TileBoundingVolumeKind::CylinderRegion;
        volume.cylinderRegion = cylinderRegion;
        return volume;
    }

    static TileBoundingVolume fromS2Cell(
        const S2CellBoundingVolume& s2Cell) {
        TileBoundingVolume volume;
        volume.kind = TileBoundingVolumeKind::S2Cell;
        volume.s2Cell = s2Cell;
        return volume;
    }

    TileBoundingVolume transform(const Mat4& matrix) const {
        switch (kind) {
            case TileBoundingVolumeKind::Region:
                return *this;
            case TileBoundingVolumeKind::Sphere: {
                TileBoundingVolume transformed = *this;
                transformed.sphere = sphere.transform(matrix);
                return transformed;
            }
            case TileBoundingVolumeKind::Box: {
                TileBoundingVolume transformed = *this;
                transformed.box = box.transform(matrix);
                return transformed;
            }
            case TileBoundingVolumeKind::CylinderRegion: {
                TileBoundingVolume transformed = *this;
                transformed.cylinderRegion =
                    cylinderRegion.transform(matrix.raw());
                return transformed;
            }
            case TileBoundingVolumeKind::S2Cell:
                return *this;
        }
        return *this;
    }

    std::optional<OrientedBoundingBox> toOrientedBoundingBox() const;
    std::optional<Rectangle> estimateGlobeRectangle() const;
    std::optional<Rectangle> estimateGlobeRectangle(
        const Ellipsoid& ellipsoid) const;
};

} // namespace earth_engine
