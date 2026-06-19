#pragma once

#include "../core/math/BoundingSphere.h"
#include "../core/math/Mat4.h"
#include "../core/math/OrientedBoundingBox.h"
#include "../core/math/Rectangle.h"

#include <optional>

namespace earth_engine {

class Ellipsoid;

enum class TileBoundingVolumeKind {
    Region,
    Sphere,
    Box
};

/// cesium-native BoundingVolume subset used by explicit 3D Tiles JSON.
/// Regions are stored in radians/meters and are not transformed by tile
/// transforms, matching cesium-native transformBoundingVolume.
struct TileBoundingVolume {
    TileBoundingVolumeKind kind = TileBoundingVolumeKind::Region;
    Rectangle region;
    double minimumHeight = 0.0;
    double maximumHeight = 0.0;
    BoundingSphere sphere{Vec3::zero(), 0.0};
    OrientedBoundingBox box{
        Vec3::zero(),
        Vec3::zero(),
        Vec3::zero(),
        Vec3::zero()};

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
        }
        return *this;
    }

    std::optional<OrientedBoundingBox> toOrientedBoundingBox() const;
    std::optional<Rectangle> estimateGlobeRectangle() const;
    std::optional<Rectangle> estimateGlobeRectangle(
        const Ellipsoid& ellipsoid) const;
};

} // namespace earth_engine
