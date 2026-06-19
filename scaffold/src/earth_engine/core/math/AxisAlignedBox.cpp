#include "AxisAlignedBox.h"

#include <algorithm>

namespace earth_engine {

AxisAlignedBox::AxisAlignedBox(double minimumX,
                               double minimumY,
                               double minimumZ,
                               double maximumX,
                               double maximumY,
                               double maximumZ)
    : minimumX_(minimumX),
      minimumY_(minimumY),
      minimumZ_(minimumZ),
      maximumX_(maximumX),
      maximumY_(maximumY),
      maximumZ_(maximumZ),
      lengthX_(maximumX - minimumX),
      lengthY_(maximumY - minimumY),
      lengthZ_(maximumZ - minimumZ),
      center_(
          0.5 * (maximumX + minimumX),
          0.5 * (maximumY + minimumY),
          0.5 * (maximumZ + minimumZ)) {}

bool AxisAlignedBox::contains(const Vec3& position) const {
    return position.x() >= minimumX_ && position.x() <= maximumX_ &&
           position.y() >= minimumY_ && position.y() <= maximumY_ &&
           position.z() >= minimumZ_ && position.z() <= maximumZ_;
}

AxisAlignedBox AxisAlignedBox::fromPositions(
    const std::vector<Vec3>& positions) {
    if (positions.empty()) {
        return AxisAlignedBox();
    }

    Vec3 minimum = positions.front();
    Vec3 maximum = positions.front();
    for (size_t i = 1; i < positions.size(); ++i) {
        const Vec3& position = positions[i];
        minimum = Vec3(
            std::min(minimum.x(), position.x()),
            std::min(minimum.y(), position.y()),
            std::min(minimum.z(), position.z()));
        maximum = Vec3(
            std::max(maximum.x(), position.x()),
            std::max(maximum.y(), position.y()),
            std::max(maximum.z(), position.z()));
    }

    return AxisAlignedBox(
        minimum.x(),
        minimum.y(),
        minimum.z(),
        maximum.x(),
        maximum.y(),
        maximum.z());
}

} // namespace earth_engine
