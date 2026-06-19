#pragma once

#include "Cartographic.h"
#include "earth_engine/core/math/Rectangle.h"

namespace earth_engine {

/// Incrementally builds a geodetic rectangle with minimum/maximum height.
///
/// This follows cesium-native BoundingRegionBuilder semantics: latitude and
/// height are always expanded, while longitude is ignored for positions that
/// are within the pole tolerance.
class BoundingRegionBuilder {
public:
    struct BoundingRegion {
        Rectangle rectangle;
        double minimumHeight = 1.0;
        double maximumHeight = -1.0;
    };

    BoundingRegionBuilder();

    BoundingRegion toRegion() const;
    Rectangle toRectangle() const;

    void setPoleTolerance(double tolerance);
    double poleTolerance() const;

    bool expandToIncludePosition(const Cartographic& position);
    bool expandToIncludeRectangle(const Rectangle& rectangle);
    bool expandToIncludeRegion(const BoundingRegion& region);

private:
    bool longitudeRangeIsEmpty_ = true;
    double poleTolerance_ = 0.0;
    double west_ = 0.0;
    double south_ = 0.0;
    double east_ = 0.0;
    double north_ = 0.0;
    double minimumHeight_ = 0.0;
    double maximumHeight_ = 0.0;
};

} // namespace earth_engine
