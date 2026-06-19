#include "BoundingRegionBuilder.h"

#include "earth_engine/core/math/MathUtils.h"

#include <cmath>
#include <limits>

namespace earth_engine {

namespace {

bool isCloseToPole(double latitude, double tolerance) {
    return MathUtils::PiOverTwo - std::abs(latitude) < tolerance;
}

} // namespace

BoundingRegionBuilder::BoundingRegionBuilder()
    : poleTolerance_(MathUtils::Epsilon10),
      west_(Rectangle::EMPTY.west()),
      south_(Rectangle::EMPTY.south()),
      east_(Rectangle::EMPTY.east()),
      north_(Rectangle::EMPTY.north()),
      minimumHeight_(std::numeric_limits<double>::max()),
      maximumHeight_(std::numeric_limits<double>::lowest()) {}

BoundingRegionBuilder::BoundingRegion BoundingRegionBuilder::toRegion() const {
    if (longitudeRangeIsEmpty_) {
        return {Rectangle::EMPTY, 1.0, -1.0};
    }
    return {Rectangle(west_, south_, east_, north_), minimumHeight_, maximumHeight_};
}

Rectangle BoundingRegionBuilder::toRectangle() const {
    if (longitudeRangeIsEmpty_) {
        return Rectangle::EMPTY;
    }
    return Rectangle(west_, south_, east_, north_);
}

void BoundingRegionBuilder::setPoleTolerance(double tolerance) {
    poleTolerance_ = tolerance;
}

double BoundingRegionBuilder::poleTolerance() const {
    return poleTolerance_;
}

bool BoundingRegionBuilder::expandToIncludePosition(
    const Cartographic& position) {
    bool modified = false;

    if (position.latitude() < south_) {
        south_ = position.latitude();
        modified = true;
    }
    if (position.latitude() > north_) {
        north_ = position.latitude();
        modified = true;
    }
    if (position.height() < minimumHeight_) {
        minimumHeight_ = position.height();
        modified = true;
    }
    if (position.height() > maximumHeight_) {
        maximumHeight_ = position.height();
        modified = true;
    }

    if (isCloseToPole(position.latitude(), poleTolerance_)) {
        return modified;
    }

    if (longitudeRangeIsEmpty_) {
        west_ = position.longitude();
        east_ = position.longitude();
        longitudeRangeIsEmpty_ = false;
        return true;
    }

    const Rectangle rectangle(west_, south_, east_, north_);
    if (rectangle.contains(position.longitude(), position.latitude())) {
        return modified;
    }

    double positionToWestDistance = west_ - position.longitude();
    if (positionToWestDistance < 0.0) {
        const double antiMeridianToWest = west_ - (-MathUtils::OnePi);
        const double positionToAntiMeridian =
            MathUtils::OnePi - position.longitude();
        positionToWestDistance = antiMeridianToWest + positionToAntiMeridian;
    }

    double eastToPositionDistance = position.longitude() - east_;
    if (eastToPositionDistance < 0.0) {
        const double antiMeridianToPosition =
            position.longitude() - (-MathUtils::OnePi);
        const double eastToAntiMeridian = MathUtils::OnePi - east_;
        eastToPositionDistance = antiMeridianToPosition + eastToAntiMeridian;
    }

    if (positionToWestDistance < eastToPositionDistance) {
        west_ = position.longitude();
    } else {
        east_ = position.longitude();
    }
    return true;
}

bool BoundingRegionBuilder::expandToIncludeRectangle(
    const Rectangle& rectangle) {
    bool modified = false;

    if (longitudeRangeIsEmpty_) {
        west_ = rectangle.west();
        east_ = rectangle.east();
        longitudeRangeIsEmpty_ = false;
        modified = true;
    }

    const Rectangle current(west_, south_, east_, north_);
    const Rectangle expanded = current.computeUnion(rectangle);
    if (expanded != current) {
        west_ = expanded.west();
        south_ = expanded.south();
        east_ = expanded.east();
        north_ = expanded.north();
        modified = true;
    }

    return modified;
}

bool BoundingRegionBuilder::expandToIncludeRegion(
    const BoundingRegion& region) {
    bool modified = expandToIncludeRectangle(region.rectangle);

    if (region.minimumHeight < minimumHeight_) {
        minimumHeight_ = region.minimumHeight;
        modified = true;
    }
    if (region.maximumHeight > maximumHeight_) {
        maximumHeight_ = region.maximumHeight;
        modified = true;
    }

    return modified;
}

} // namespace earth_engine
