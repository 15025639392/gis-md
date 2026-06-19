#pragma once

#include "S2CellID.h"

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

private:
    S2CellID cellID_;
    double minimumHeight_ = 0.0;
    double maximumHeight_ = 0.0;
};

} // namespace earth_engine
