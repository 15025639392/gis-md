#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

#include "earth_engine/data/StyleExpression.h"

namespace earth_engine {

// Current JSAPI StyleParser.dn contract: floor while fraction < .8,
// otherwise ceil. PBF record builders store stops at (amapZoom - 1).
inline StyleExpression::Ptr amapClassicDiscreteZoom() {
    return StyleExpression::discreteZoom(0.8);
}

inline double amapClassicDiscreteZoomValue(double zoom) {
    if (!std::isfinite(zoom)) return zoom;
    const double lo = std::floor(zoom);
    const double tolerance =
        8.0 * std::numeric_limits<double>::epsilon() *
        std::max(1.0, std::abs(zoom));
    return zoom - lo + tolerance < 0.8 ? lo : std::ceil(zoom);
}

}  // namespace earth_engine
