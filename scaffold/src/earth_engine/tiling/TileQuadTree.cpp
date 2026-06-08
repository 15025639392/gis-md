#include "TileQuadTree.h"

#include "TilePlan.h"
#include "TileScheme.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/math/Ray.h"
#include "../core/math/Vec3.h"
#include "../scene/Camera.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace earth_engine {
namespace {

constexpr double kEarthRadius = 6378137.0;
constexpr double kCurrentLodPixels = 256.0;
constexpr double kMinLodPixels = 512.0;
constexpr double kMaxLodPixels = 256.0;
constexpr int kAlwaysSubdivideUntilZoom = 2;
constexpr size_t kMaxRenderedNodes = 1000;
constexpr double kOpenGlobusMaxHorizonDistanceSquared = 106876472875.63281;

double normalizeLongitude(double lngRad) {
    double x = std::fmod(lngRad + glm::pi<double>(), glm::two_pi<double>());
    if (x < 0.0) x += glm::two_pi<double>();
    return x - glm::pi<double>();
}

std::vector<Vec3> tileSamplePoints(const Rectangle& bounds) {
    const auto& ellipsoid = Ellipsoid::WGS84();
    const double west = bounds.west();
    const double east = bounds.east();
    const double south = bounds.south();
    const double north = bounds.north();
    const double midLng = normalizeLongitude(west + (east - west) * 0.5);
    const double midLat = (south + north) * 0.5;

    std::vector<Vec3> points;
    points.reserve(9);
    const double lngs[] = {west, midLng, east};
    const double lats[] = {south, midLat, north};
    for (double lat : lats) {
        for (double lng : lngs) {
            points.push_back(ellipsoid.cartographicToCartesian(
                Cartographic::fromRadians(lng, lat, 0.0)));
        }
    }
    return points;
}

std::pair<Vec3, double> boundingSphereFor(const Rectangle& bounds) {
    const auto points = tileSamplePoints(bounds);
    Vec3 center = Vec3::zero();
    for (const Vec3& p : points) {
        center += p;
    }
    center = center / static_cast<double>(points.size());

    double radius = 0.0;
    for (const Vec3& p : points) {
        radius = std::max(radius, center.distanceTo(p));
    }
    return {center, radius};
}

std::array<Vec3, 4> tileCornerPoints(const Rectangle& bounds) {
    const auto& ellipsoid = Ellipsoid::WGS84();
    return {
        ellipsoid.cartographicToCartesian(
            Cartographic::fromRadians(bounds.west(), bounds.south(), 0.0)),
        ellipsoid.cartographicToCartesian(
            Cartographic::fromRadians(bounds.west(), bounds.north(), 0.0)),
        ellipsoid.cartographicToCartesian(
            Cartographic::fromRadians(bounds.east(), bounds.north(), 0.0)),
        ellipsoid.cartographicToCartesian(
            Cartographic::fromRadians(bounds.east(), bounds.south(), 0.0))
    };
}

double distanceSquared(const Vec3& a, const Vec3& b) {
    return (a - b).lengthSquared();
}

} // namespace

TileNode::TileNode(TileKey key, Rectangle bounds, TileNode* parent)
    : key_(std::move(key)), bounds_(bounds), parent_(parent) {
    auto sphere = boundingSphereFor(bounds_);
    boundingCenter_ = sphere.first;
    boundingRadiusMeters_ = sphere.second;
    cornerPoints_ = tileCornerPoints(bounds_);
}

void TileNode::resetFrameState() {
    state_ = TileNodeState::NotRendering;
    cameraInside_ = false;
    for (auto& child : children_) {
        if (child) child->resetFrameState();
    }
}

void TileNode::ensureChildren(const TileScheme& scheme) {
    if (children_[0]) return;

    const int childZ = key_.z + 1;
    const int childX = key_.x * 2;
    const int childY = key_.y * 2;
    children_[0] = std::make_unique<TileNode>(
        TileKey{key_.schemeId, childZ, childX, childY},
        scheme.tileToRectangle(TileKey{key_.schemeId, childZ, childX, childY}),
        this);
    children_[1] = std::make_unique<TileNode>(
        TileKey{key_.schemeId, childZ, childX + 1, childY},
        scheme.tileToRectangle(TileKey{key_.schemeId, childZ, childX + 1, childY}),
        this);
    children_[2] = std::make_unique<TileNode>(
        TileKey{key_.schemeId, childZ, childX, childY + 1},
        scheme.tileToRectangle(TileKey{key_.schemeId, childZ, childX, childY + 1}),
        this);
    children_[3] = std::make_unique<TileNode>(
        TileKey{key_.schemeId, childZ, childX + 1, childY + 1},
        scheme.tileToRectangle(TileKey{key_.schemeId, childZ, childX + 1, childY + 1}),
        this);
}

bool TileNode::containsCartographic(double longitudeRad, double latitudeRad) const {
    return bounds_.contains(longitudeRad, latitudeRad);
}

int TileNode::subtreeNodeCount() const {
    int count = 1;
    for (const auto& child : children_) {
        if (child) count += child->subtreeNodeCount();
    }
    return count;
}

bool TileNode::isAltitudeVisible(const Camera& camera) const {
    if (key_.z < 2 || key_.z > 19) return true;

    // OpenGlobus keeps low plain-terrain segments traversable while terrain is
    // not ready. This MVP has ellipsoid SurfaceTile geometry only, so the
    // matching no-terrain path treats z<6 as altitude-visible.
    if (key_.z < 6) return true;

    const double polarRadius = Ellipsoid::WGS84().semiMinorAxis();
    const double horizonDistanceSquared = std::max(
        camera.position().lengthSquared() - polarRadius * polarRadius,
        kOpenGlobusMaxHorizonDistanceSquared);

    for (const Vec3& corner : cornerPoints_) {
        if (distanceSquared(camera.position(), corner) < horizonDistanceSquared) {
            return true;
        }
    }
    return false;
}

bool TileNode::shouldSubdivide(const Camera& camera,
                               double viewportWidthPixels,
                               double viewportHeightPixels) const {
    const double distance = std::max(1.0, camera.position().distanceTo(boundingCenter_));
    const double viewport = std::min(
        viewportWidthPixels < 512.0 ? 512.0 : viewportWidthPixels,
        viewportHeightPixels < 512.0 ? 512.0 : viewportHeightPixels);
    const double projectedSizePixels =
        std::atan(boundingRadiusMeters_ / distance) *
        viewport / camera.verticalFovRadians();

    const double slope = std::clamp(
        (-camera.direction()).dot(camera.position().normalized()),
        0.0,
        1.0);
    const double lodPixels = kCurrentLodPixels +
        (kMinLodPixels - kCurrentLodPixels) * slope;

    const double refineThreshold = std::clamp(lodPixels, kMaxLodPixels, kMinLodPixels);
    return projectedSizePixels > refineThreshold;
}

void TileNode::traverse(const TileScheme& scheme,
                        const Camera& camera,
                        double viewportWidthPixels,
                        double viewportHeightPixels,
                        double cameraLongitudeRad,
                        double cameraLatitudeRad,
                        bool parentCameraInside,
                        size_t maxRenderedTiles,
                        std::vector<TileKey>& out) {
    if (out.size() >= maxRenderedTiles) {
        return;
    }

    const Frustum frustum = camera.frustum(viewportWidthPixels, viewportHeightPixels);
    cameraInside_ = parentCameraInside &&
                    containsCartographic(cameraLongitudeRad, cameraLatitudeRad);
    const bool visible = frustum.intersectsSphere(boundingCenter_, boundingRadiusMeters_) ||
                         cameraInside_ ||
                         key_.z < 3;
    const bool altVisible = isAltitudeVisible(camera);
    if (!visible) {
        state_ = TileNodeState::NotRendering;
        return;
    }

    const bool canSubdivide = key_.z < scheme.maxZoom();
    const bool mustSubdivide = key_.z < std::max(scheme.minZoom(), kAlwaysSubdivideUntilZoom);
    const bool refine = mustSubdivide || (canSubdivide && shouldSubdivide(
        camera, viewportWidthPixels, viewportHeightPixels));
    if (!altVisible && !cameraInside_) {
        state_ = TileNodeState::NotRendering;
        return;
    }

    if (!refine || !canSubdivide) {
        state_ = TileNodeState::Rendering;
        out.push_back(key_);
        return;
    }

    state_ = TileNodeState::Walkthrough;
    ensureChildren(scheme);
    for (auto& child : children_) {
        child->traverse(scheme,
                        camera,
                        viewportWidthPixels,
                        viewportHeightPixels,
                        cameraLongitudeRad,
                        cameraLatitudeRad,
                        cameraInside_,
                        maxRenderedTiles,
                        out);
    }
}

void TileQuadTree::resetIfSchemeChanged(const TileScheme& scheme) {
    if (schemeId_ == scheme.id()) return;
    root_.reset();
    schemeId_ = scheme.id();
    createdNodeCount_ = 0;
}

void TileQuadTree::ensureRoot(const TileScheme& scheme) {
    resetIfSchemeChanged(scheme);
    if (root_) return;
    TileKey rootKey{scheme.id(), 0, 0, 0};
    root_ = std::make_unique<TileNode>(
        rootKey, scheme.tileToRectangle(rootKey), nullptr);
    createdNodeCount_ = 1;
}

TilePlan TileQuadTree::compute(const Camera& camera,
                               const TileScheme& scheme,
                               double viewportWidthPixels,
                               double viewportHeightPixels,
                               int previousZoom) {
    TilePlan plan;
    ensureRoot(scheme);
    root_->resetFrameState();

    Vec3 camPos = camera.position();
    double cameraHeight = camPos.length() - kEarthRadius;
    if (cameraHeight < 1000.0) cameraHeight = 1000.0;

    (void)previousZoom;
    const Cartographic subCamera = Ellipsoid::WGS84().cartesianToCartographic(
        camera.position().normalized() * kEarthRadius);

    root_->traverse(scheme,
                    camera,
                    viewportWidthPixels,
                    viewportHeightPixels,
                    subCamera.longitude(),
                    subCamera.latitude(),
                    true,
                    kMaxRenderedNodes,
                    plan.visibleTiles);

    std::unordered_set<TileKey> seen;
    std::vector<TileKey> deduped;
    deduped.reserve(plan.visibleTiles.size());
    for (const TileKey& key : plan.visibleTiles) {
        if (seen.insert(key).second) {
            deduped.push_back(key);
        }
    }
    plan.visibleTiles = std::move(deduped);
    plan.zoom = 0;
    for (const TileKey& key : plan.visibleTiles) {
        plan.zoom = std::max(plan.zoom, key.z);
    }

    createdNodeCount_ = root_ ? root_->subtreeNodeCount() : 0;
    lastVisitedNodeCount_ = static_cast<int>(plan.visibleTiles.size());
    return plan;
}

} // namespace earth_engine
