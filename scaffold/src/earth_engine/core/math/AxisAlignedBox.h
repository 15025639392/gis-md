#pragma once

#include "Vec3.h"

#include <vector>

namespace earth_engine {

/// Axis-aligned bounding box aligned with cesium-native AxisAlignedBox.
class AxisAlignedBox {
public:
    AxisAlignedBox() = default;
    AxisAlignedBox(double minimumX,
                   double minimumY,
                   double minimumZ,
                   double maximumX,
                   double maximumY,
                   double maximumZ);

    double minimumX() const { return minimumX_; }
    double minimumY() const { return minimumY_; }
    double minimumZ() const { return minimumZ_; }
    double maximumX() const { return maximumX_; }
    double maximumY() const { return maximumY_; }
    double maximumZ() const { return maximumZ_; }

    double lengthX() const { return lengthX_; }
    double lengthY() const { return lengthY_; }
    double lengthZ() const { return lengthZ_; }
    const Vec3& center() const { return center_; }

    bool contains(const Vec3& position) const;

    static AxisAlignedBox fromPositions(const std::vector<Vec3>& positions);

private:
    double minimumX_ = 0.0;
    double minimumY_ = 0.0;
    double minimumZ_ = 0.0;
    double maximumX_ = 0.0;
    double maximumY_ = 0.0;
    double maximumZ_ = 0.0;
    double lengthX_ = 0.0;
    double lengthY_ = 0.0;
    double lengthZ_ = 0.0;
    Vec3 center_ = Vec3::zero();
};

} // namespace earth_engine
