#pragma once

#include "Cartographic.h"
#include "../math/Rectangle.h"

namespace earth_engine {

class Gcj02CoordinateTransform {
public:
    static bool isOutsideChina(const Cartographic& cartographic);

    /// Converts a WGS84 cartographic position to GCJ-02. Positions outside
    /// mainland China's GCJ transform bounds are returned unchanged.
    static Cartographic fromWgs84(const Cartographic& cartographic);

    /// Computes a conservative GCJ-02 longitude/latitude envelope for a WGS84
    /// rectangle. Outside-China rectangles are returned bit-for-bit unchanged.
    static Rectangle boundRectangleFromWgs84(const Rectangle& rectangle);
};

} // namespace earth_engine
