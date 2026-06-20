#pragma once

#include "Cartographic.h"
#include "Ellipsoid.h"
#include "S2CellID.h"
#include "../math/Vec3.h"

namespace earth_engine {

class S2CellBoundingVolume {
public:
    S2CellBoundingVolume(const S2CellID& cellID,
                         double minimumHeight,
                         double maximumHeight) noexcept
        : cellID_(cellID),
          minimumHeight_(minimumHeight),
          maximumHeight_(maximumHeight) {}

    const S2CellID& getCellID() const noexcept { return cellID_; }
    double getMinimumHeight() const noexcept { return minimumHeight_; }
    double getMaximumHeight() const noexcept { return maximumHeight_; }
    Vec3 getCenter() const {
        Cartographic center = cellID_.getCenter();
        return Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic::fromRadians(
                center.longitude(),
                center.latitude(),
                (minimumHeight_ + maximumHeight_) * 0.5));
    }

private:
    S2CellID cellID_;
    double minimumHeight_ = 0.0;
    double maximumHeight_ = 0.0;
};

} // namespace earth_engine
