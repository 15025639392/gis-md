#pragma once

#include "S2CellID.h"
#include "../math/Plane.h"
#include "../math/Vec3.h"

#include <array>

namespace earth_engine {

class S2CellBoundingVolume {
public:
    S2CellBoundingVolume(const S2CellID& cellID,
                         double minimumHeight,
                         double maximumHeight);

    const S2CellID& getCellID() const noexcept { return cellID_; }
    double getMinimumHeight() const noexcept { return minimumHeight_; }
    double getMaximumHeight() const noexcept { return maximumHeight_; }
    const Vec3& getCenter() const noexcept { return center_; }
    const std::array<Vec3, 8>& getVertices() const noexcept { return vertices_; }
    const std::array<Plane, 6>& getBoundingPlanes() const noexcept {
        return boundingPlanes_;
    }

    double computeDistanceSquaredToPosition(const Vec3& position) const noexcept;
    bool contains(const Vec3& position) const noexcept {
        return computeDistanceSquaredToPosition(position) <= 1e-8;
    }

private:
    S2CellID cellID_;
    double minimumHeight_ = 0.0;
    double maximumHeight_ = 0.0;
    Vec3 center_;
    std::array<Plane, 6> boundingPlanes_;
    std::array<Vec3, 8> vertices_;
};

} // namespace earth_engine
