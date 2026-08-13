#pragma once

#include "Cartographic.h"
#include "../math/Rectangle.h"

namespace earth_engine {

class Gcj02CoordinateTransform {
public:
    static bool isOutsideChina(const Cartographic& cartographic);

    /// True when the rectangle straddles the GCJ transform bounds, i.e. it
    /// contains both transformed and untransformed positions. Rectangles
    /// wholly inside or wholly outside the bounds return false: a single
    /// per-rectangle offset represents them to within the warp's own gradient,
    /// whereas a straddling rectangle has a ~500 m step across it that no
    /// single offset can represent.
    static bool crossesChinaBounds(const Rectangle& rectangle);

    /// Converts a WGS84 cartographic position to GCJ-02. Positions outside
    /// mainland China's GCJ transform bounds are returned unchanged.
    static Cartographic fromWgs84(const Cartographic& cartographic);

    /// Inverse of fromWgs84: recovers the WGS84 position for a GCJ-02
    /// position via fixed-point iteration (the warp's gradient is ~1e-5, so
    /// two rounds converge to sub-millimetre). The inside-China test is
    /// evaluated on the GCJ input — asymmetric with fromWgs84 by up to the
    /// warp magnitude (~500 m) right at the bounds; callers dealing with
    /// straddling rectangles should consult crossesChinaBounds, same as the
    /// forward direction.
    static Cartographic toWgs84(const Cartographic& cartographic);

    /// Computes a conservative GCJ-02 longitude/latitude envelope for a WGS84
    /// rectangle. Outside-China rectangles are returned bit-for-bit unchanged.
    static Rectangle boundRectangleFromWgs84(const Rectangle& rectangle);
};

} // namespace earth_engine
