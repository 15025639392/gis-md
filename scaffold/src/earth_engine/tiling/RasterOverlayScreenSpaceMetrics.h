#pragma once

#include "../core/math/Rectangle.h"

namespace earth_engine {

struct RasterTargetScreenPixels {
    double x = 256.0;
    double y = 256.0;
};

class RasterOverlayScreenSpaceMetrics {
public:
    static RasterTargetScreenPixels computeDesiredScreenPixels(
        const Rectangle& bounds,
        double geometricError,
        double maximumScreenSpaceError);
};

} // namespace earth_engine
