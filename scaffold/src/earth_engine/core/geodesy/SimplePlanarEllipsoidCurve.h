#pragma once

#include "Cartographic.h"
#include "Ellipsoid.h"
#include "../math/Vec3.h"

#include <optional>

namespace earth_engine {

/// Produces points on an ellipsoid-centered planar curve between two positions.
///
/// Aligned with cesium-native CesiumGeospatial::SimplePlanarEllipsoidCurve:
/// positions follow the great-circle-like plane through the ellipsoid center,
/// while height is linearly interpolated between the original endpoints.
class SimplePlanarEllipsoidCurve {
public:
    static std::optional<SimplePlanarEllipsoidCurve>
    fromEarthCenteredEarthFixedCoordinates(const Ellipsoid& ellipsoid,
                                           const Vec3& sourceEcef,
                                           const Vec3& destinationEcef);

    static std::optional<SimplePlanarEllipsoidCurve>
    fromLongitudeLatitudeHeight(const Ellipsoid& ellipsoid,
                                const Cartographic& source,
                                const Cartographic& destination);

    Vec3 getPosition(double percentage, double additionalHeight = 0.0) const;

private:
    SimplePlanarEllipsoidCurve(const Ellipsoid& ellipsoid,
                               const Vec3& scaledSourceEcef,
                               const Vec3& scaledDestinationEcef,
                               const Vec3& originalSourceEcef,
                               const Vec3& originalDestinationEcef);

    double totalAngle_ = 0.0;
    double sourceHeight_ = 0.0;
    double destinationHeight_ = 0.0;
    Ellipsoid ellipsoid_;
    Vec3 sourceDirection_;
    Vec3 rotationAxis_;
    Vec3 sourceEcef_;
    Vec3 destinationEcef_;
};

} // namespace earth_engine
