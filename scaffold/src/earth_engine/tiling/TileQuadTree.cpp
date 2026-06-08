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
constexpr double kOpenGlobusCurrentLodPixels = 256.0;
constexpr double kOpenGlobusMinLodPixels = 512.0;
constexpr double kOpenGlobusMaxLodPixels = 256.0;
constexpr double kOpenGlobusMinEqualZoomAltitudeMeters = 10000.0;
constexpr double kOpenGlobusMaxEqualZoomAltitudeMeters = 15000000.0;
constexpr double kOpenGlobusMinEqualZoomCameraSlope = 0.8;
constexpr double kOpenGlobusHorizonTangent = 0.81;
constexpr int kAlwaysSubdivideUntilZoom = 2;
constexpr size_t kMaxRenderedNodes = 1000;
constexpr double kOpenGlobusMaxHorizonDistanceSquared = 106876472875.63281;
constexpr int kTileSizePixels = 256;

int openGlobusGroupBaseY(int group, int tilesAtZoom) {
    return group * tilesAtZoom;
}

int openGlobusGroupForY(int y, int tilesAtZoom) {
    if (y >= 2 * tilesAtZoom) return 2;
    if (y >= tilesAtZoom) return 1;
    return 0;
}

int childYForTile(const TileKey& key, int childLocalYOffset) {
    if (key.schemeId != "OpenGlobus-Earth") {
        return key.y * 2 + childLocalYOffset;
    }

    const int tilesAtZoom = 1 << key.z;
    const int childTilesAtZoom = 1 << (key.z + 1);
    const int group = openGlobusGroupForY(key.y, tilesAtZoom);
    const int localY = std::clamp(
        key.y - openGlobusGroupBaseY(group, tilesAtZoom),
        0,
        tilesAtZoom - 1);
    return openGlobusGroupBaseY(group, childTilesAtZoom) +
           localY * 2 +
           childLocalYOffset;
}

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

void accumulateNodeStats(const TileNode* node, TilePlan& plan) {
    if (!node) return;
    switch (node->state()) {
        case TileNodeState::Rendering:
            ++plan.renderingNodeCount;
            break;
        case TileNodeState::Walkthrough:
            ++plan.walkthroughNodeCount;
            break;
        case TileNodeState::NotRendering:
            ++plan.notRenderingNodeCount;
            break;
    }
    if (node->cameraInside()) {
        ++plan.cameraInsideNodeCount;
    }
    if (node->inFrustumMask() != 0) {
        ++plan.inFrustumNodeCount;
    }
    plan.fadingNodeCount += node->fadingNodeCount();
    for (const auto& child : node->children()) {
        accumulateNodeStats(child.get(), plan);
    }
}

void accumulateTileGroupStats(TilePlan& plan) {
    for (const TileKey& key : plan.visibleTiles) {
        if (key.schemeId == "OpenGlobus-Earth") {
            const int tilesAtZoom = 1 << key.z;
            if (key.y >= 2 * tilesAtZoom) {
                ++plan.southPolarTileCount;
            } else if (key.y >= tilesAtZoom) {
                ++plan.northPolarTileCount;
            } else {
                ++plan.mercatorTileCount;
            }
        } else {
            ++plan.mercatorTileCount;
        }
    }
}

double cameraSlope(const Camera& camera) {
    return std::clamp(
        (-camera.direction()).dot(camera.position().normalized()),
        0.0,
        1.0);
}

double openglobusLodSizePixels(const Camera& camera) {
    const double slope = cameraSlope(camera);
    return kOpenGlobusCurrentLodPixels +
        (kOpenGlobusMinLodPixels - kOpenGlobusCurrentLodPixels) * slope;
}

double projectedSizePixels(const Camera& camera,
                           const Vec3& center,
                           double radiusMeters,
                           double viewportWidthPixels,
                           double viewportHeightPixels) {
    const double distance = std::max(1.0, camera.position().distanceTo(center));
    const double viewport = std::min(
        viewportWidthPixels < 512.0 ? 512.0 : viewportWidthPixels,
        viewportHeightPixels < 512.0 ? 512.0 : viewportHeightPixels);
    const double projSizeConst = viewport / camera.verticalFovRadians();
    return std::atan(radiusMeters / distance) * projSizeConst;
}

bool shouldApplyEqualZoom(const Camera& camera, double cameraHeightMeters) {
    return cameraSlope(camera) > kOpenGlobusMinEqualZoomCameraSlope &&
           cameraHeightMeters < kOpenGlobusMaxEqualZoomAltitudeMeters &&
           cameraHeightMeters > kOpenGlobusMinEqualZoomAltitudeMeters;
}

int zoomLevelFromHeight(double cameraHeightMeters,
                        double viewportHeightPixels,
                        double verticalFovRadians,
                        int minZoom,
                        int maxZoom) {
    if (viewportHeightPixels <= 0.0) return minZoom;
    const double safeHeight = std::max(1.0, cameraHeightMeters);
    const double metersPerPixel = safeHeight * 2.0 *
                                  std::tan(verticalFovRadians * 0.5) /
                                  viewportHeightPixels;
    const double earthCircumference = 2.0 * glm::pi<double>() * kEarthRadius;
    const double idealTiles =
        earthCircumference / (static_cast<double>(kTileSizePixels) * metersPerPixel);
    const int zoom = static_cast<int>(std::round(std::log2(idealTiles)));
    return std::clamp(zoom, minZoom, maxZoom);
}

void dedupeAndUpdateZoomStats(TilePlan& plan) {
    std::unordered_set<TileKey> seen;
    std::vector<TileKey> deduped;
    deduped.reserve(plan.visibleTiles.size());
    for (const TileKey& key : plan.visibleTiles) {
        if (seen.insert(key).second) {
            deduped.push_back(key);
        }
    }
    plan.visibleTiles = std::move(deduped);

    if (plan.visibleTiles.empty()) {
        plan.zoom = 0;
        plan.minVisibleZoom = 0;
        plan.maxVisibleZoom = 0;
        return;
    }

    plan.minVisibleZoom = plan.visibleTiles.front().z;
    plan.maxVisibleZoom = plan.visibleTiles.front().z;
    for (const TileKey& key : plan.visibleTiles) {
        plan.minVisibleZoom = std::min(plan.minVisibleZoom, key.z);
        plan.maxVisibleZoom = std::max(plan.maxVisibleZoom, key.z);
    }
    plan.zoom = plan.maxVisibleZoom;
}

bool haveCommonSide(const TileNode& a, const TileNode& b) {
    constexpr double kEpsilon = 1e-12;
    const Rectangle& ar = a.bounds();
    const Rectangle& br = b.bounds();

    const bool latOverlap =
        ar.south() <= br.north() + kEpsilon &&
        ar.north() + kEpsilon >= br.south();
    const bool lngOverlap =
        ar.west() <= br.east() + kEpsilon &&
        ar.east() + kEpsilon >= br.west();

    if (std::abs(ar.east() - br.west()) <= kEpsilon && latOverlap) return true;
    if (std::abs(ar.west() - br.east()) <= kEpsilon && latOverlap) return true;
    if (std::abs(ar.north() - br.south()) <= kEpsilon && lngOverlap) return true;
    if (std::abs(ar.south() - br.north()) <= kEpsilon && lngOverlap) return true;

    const bool antimeridianEast =
        std::abs(ar.east() - glm::pi<double>()) <= kEpsilon &&
        std::abs(br.west() + glm::pi<double>()) <= kEpsilon;
    const bool antimeridianWest =
        std::abs(ar.west() + glm::pi<double>()) <= kEpsilon &&
        std::abs(br.east() - glm::pi<double>()) <= kEpsilon;
    return (antimeridianEast || antimeridianWest) && latOverlap;
}

void accumulateNeighborStats(const std::vector<TileNode*>& renderedNodes, TilePlan& plan) {
    int links = 0;
    for (size_t i = 0; i < renderedNodes.size(); ++i) {
        for (size_t j = i + 1; j < renderedNodes.size(); ++j) {
            if (haveCommonSide(*renderedNodes[i], *renderedNodes[j])) {
                ++links;
            }
        }
    }
    plan.neighborLinkCount = links;
}

void updateTileTransitions(const std::vector<TileNode*>& renderedNodes, TilePlan& plan) {
    plan.tileTransitions.clear();
    plan.tileTransitions.reserve(renderedNodes.size());
    std::unordered_set<TileKey> seen;
    for (const TileNode* node : renderedNodes) {
        if (!node) continue;
        const TileKey& key = node->key();
        if (!seen.insert(key).second) continue;
        plan.tileTransitions.push_back(TileTransition{
            key,
            static_cast<float>(std::clamp(node->transitionOpacity(), 0.0, 1.0)),
            node->fadingNodeCount()
        });
    }
}

void applyOpenGlobusEqualZoomPass(const TileScheme& scheme,
                                  const Camera& camera,
                                  const Frustum& frustum,
                                  double viewportWidthPixels,
                                  double viewportHeightPixels,
                                  double cameraLongitudeRad,
                                  double cameraLatitudeRad,
                                  TilePlan& plan,
                                  std::vector<TileNode*>& renderedNodes) {
    if (renderedNodes.empty()) return;

    const int maxZoom = plan.maxVisibleZoom;
    std::vector<TileNode*> firstPassNodes = renderedNodes;
    std::vector<TileKey> secondPassTiles;
    std::vector<TileNode*> secondPassNodes;
    secondPassTiles.reserve(plan.visibleTiles.size());
    secondPassNodes.reserve(renderedNodes.size());

    for (TileNode* node : firstPassNodes) {
        if (!node) continue;
        const bool preserveHorizonTangent =
            node->key().z != maxZoom && node->isHorizonTangent(camera);
        if (node->key().z == maxZoom || preserveHorizonTangent) {
            secondPassTiles.push_back(node->key());
            secondPassNodes.push_back(node);
            if (preserveHorizonTangent) {
                ++plan.horizonTangentPreservedCount;
            }
            continue;
        }

        node->renderToZoom(scheme,
                           camera,
                           frustum,
                           viewportWidthPixels,
                           viewportHeightPixels,
                           cameraLongitudeRad,
                           cameraLatitudeRad,
                           node->cameraInside(),
                           maxZoom,
                           true,
                           kMaxRenderedNodes,
                           secondPassTiles,
                           secondPassNodes);
        ++plan.equalZoomSecondPassNodeCount;
    }

    if (!secondPassTiles.empty()) {
        plan.visibleTiles = std::move(secondPassTiles);
        renderedNodes = std::move(secondPassNodes);
        plan.equalZoomApplied = true;
    }
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
    previousState_ = state_;
    state_ = TileNodeState::NotRendering;
    cameraInside_ = false;
    inFrustumMask_ = 0;
    fadingNodeCount_ = 0;
    for (auto& child : children_) {
        if (child) child->resetFrameState();
    }
}

void TileNode::ensureChildren(const TileScheme& scheme) {
    if (children_[0]) return;

    const int childZ = key_.z + 1;
    const int childX = key_.x * 2;
    const int childY0 = childYForTile(key_, 0);
    const int childY1 = childYForTile(key_, 1);
    children_[0] = std::make_unique<TileNode>(
        TileKey{key_.schemeId, childZ, childX, childY0},
        scheme.tileToRectangle(TileKey{key_.schemeId, childZ, childX, childY0}),
        this);
    children_[1] = std::make_unique<TileNode>(
        TileKey{key_.schemeId, childZ, childX + 1, childY0},
        scheme.tileToRectangle(TileKey{key_.schemeId, childZ, childX + 1, childY0}),
        this);
    children_[2] = std::make_unique<TileNode>(
        TileKey{key_.schemeId, childZ, childX, childY1},
        scheme.tileToRectangle(TileKey{key_.schemeId, childZ, childX, childY1}),
        this);
    children_[3] = std::make_unique<TileNode>(
        TileKey{key_.schemeId, childZ, childX + 1, childY1},
        scheme.tileToRectangle(TileKey{key_.schemeId, childZ, childX + 1, childY1}),
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

bool TileNode::isHorizonTangent(const Camera& camera) const {
    return boundingCenter_.normalized().dot(-camera.direction()) < kOpenGlobusHorizonTangent;
}

bool TileNode::shouldSubdivide(const Camera& camera,
                               double viewportWidthPixels,
                               double viewportHeightPixels) const {
    const double projectedPixels = projectedSizePixels(
        camera,
        boundingCenter_,
        boundingRadiusMeters_,
        viewportWidthPixels,
        viewportHeightPixels);
    const double refineThreshold = std::clamp(
        openglobusLodSizePixels(camera),
        kOpenGlobusMaxLodPixels,
        kOpenGlobusMinLodPixels);
    return projectedPixels > refineThreshold;
}

bool TileNode::childrenPreviousStateEquals(TileNodeState state) const {
    return children_[0] &&
           children_[1] &&
           children_[2] &&
           children_[3] &&
           children_[0]->previousState_ == state &&
           children_[1]->previousState_ == state &&
           children_[2]->previousState_ == state &&
           children_[3]->previousState_ == state;
}

void TileNode::markRenderingTransition() {
    if (key_.z < 3) {
        transitionOpacity_ = 1.0;
        fadingNodeCount_ = 0;
        return;
    }

    if (previousState_ == TileNodeState::Rendering) {
        transitionOpacity_ = 1.0;
        fadingNodeCount_ = 0;
        return;
    }

    transitionOpacity_ = 0.0;
    fadingNodeCount_ = 0;
    if (parent_ && parent_->previousState_ == TileNodeState::Rendering) {
        fadingNodeCount_ = 1;
        return;
    }
    if (childrenPreviousStateEquals(TileNodeState::Rendering)) {
        fadingNodeCount_ = 4;
    }
}

void TileNode::traverse(const TileScheme& scheme,
                        const Camera& camera,
                        const Frustum& frustum,
                        double viewportWidthPixels,
                        double viewportHeightPixels,
                        double cameraLongitudeRad,
                        double cameraLatitudeRad,
                        bool parentCameraInside,
                        int cameraInsideTargetZoom,
                        size_t maxRenderedTiles,
                        std::vector<TileKey>& out,
                        std::vector<TileNode*>& renderedNodes) {
    if (out.size() >= maxRenderedTiles) {
        return;
    }

    cameraInside_ = parentCameraInside &&
                    containsCartographic(cameraLongitudeRad, cameraLatitudeRad);
    if (frustum.intersectsSphere(boundingCenter_, boundingRadiusMeters_)) {
        inFrustumMask_ = 1;
    }
    const bool visible = inFrustumMask_ || cameraInside_ || key_.z < 3;
    const bool altVisible = isAltitudeVisible(camera);
    if (!visible) {
        state_ = TileNodeState::NotRendering;
        return;
    }

    const bool canSubdivide = key_.z < scheme.maxZoom();
    const bool mustSubdivide = key_.z < std::max(scheme.minZoom(), kAlwaysSubdivideUntilZoom);
    const bool cameraInsideNeedsHeightZoom =
        cameraInside_ && key_.z < cameraInsideTargetZoom;
    const bool refine = mustSubdivide || (canSubdivide && shouldSubdivide(
        camera, viewportWidthPixels, viewportHeightPixels)) ||
        (canSubdivide && cameraInsideNeedsHeightZoom);
    if (!altVisible && !cameraInside_) {
        state_ = TileNodeState::NotRendering;
        return;
    }

    if (!refine || !canSubdivide) {
        state_ = TileNodeState::Rendering;
        markRenderingTransition();
        out.push_back(key_);
        renderedNodes.push_back(this);
        return;
    }

    state_ = TileNodeState::Walkthrough;
    ensureChildren(scheme);
    for (auto& child : children_) {
        child->traverse(scheme,
                        camera,
                        frustum,
                        viewportWidthPixels,
                        viewportHeightPixels,
                        cameraLongitudeRad,
                        cameraLatitudeRad,
                        cameraInside_,
                        cameraInsideTargetZoom,
                        maxRenderedTiles,
                        out,
                        renderedNodes);
    }
}

void TileNode::renderToZoom(const TileScheme& scheme,
                            const Camera& camera,
                            const Frustum& frustum,
                            double viewportWidthPixels,
                            double viewportHeightPixels,
                            double cameraLongitudeRad,
                            double cameraLatitudeRad,
                            bool parentCameraInside,
                            int targetZoom,
                            bool stopAtHorizon,
                            size_t maxRenderedTiles,
                            std::vector<TileKey>& out,
                            std::vector<TileNode*>& renderedNodes) {
    if (out.size() >= maxRenderedTiles) return;
    cameraInside_ = parentCameraInside &&
                    containsCartographic(cameraLongitudeRad, cameraLatitudeRad);
    inFrustumMask_ = frustum.intersectsSphere(boundingCenter_, boundingRadiusMeters_) ? 1 : 0;
    const bool visible = inFrustumMask_ || cameraInside_ || key_.z < 3;
    if (!visible) {
        state_ = TileNodeState::NotRendering;
        return;
    }
    if (stopAtHorizon && !isAltitudeVisible(camera) && !cameraInside_) {
        state_ = TileNodeState::NotRendering;
        return;
    }

    if (key_.z >= targetZoom || key_.z >= scheme.maxZoom()) {
        state_ = TileNodeState::Rendering;
        markRenderingTransition();
        out.push_back(key_);
        renderedNodes.push_back(this);
        return;
    }

    state_ = TileNodeState::Walkthrough;
    ensureChildren(scheme);
    for (auto& child : children_) {
        child->renderToZoom(scheme,
                            camera,
                            frustum,
                            viewportWidthPixels,
                            viewportHeightPixels,
                            cameraLongitudeRad,
                            cameraLatitudeRad,
                            cameraInside_,
                            targetZoom,
                            stopAtHorizon,
                            maxRenderedTiles,
                            out,
                            renderedNodes);
    }
}

void TileQuadTree::resetIfSchemeChanged(const TileScheme& scheme) {
    if (schemeId_ == scheme.id()) return;
    roots_.clear();
    schemeId_ = scheme.id();
    createdNodeCount_ = 0;
}

void TileQuadTree::ensureRoot(const TileScheme& scheme) {
    resetIfSchemeChanged(scheme);
    if (!roots_.empty()) return;

    if (scheme.id() == "OpenGlobus-Earth") {
        for (int rootY = 0; rootY < 3; ++rootY) {
            TileKey rootKey{scheme.id(), 0, 0, rootY};
            roots_.push_back(std::make_unique<TileNode>(
                rootKey, scheme.tileToRectangle(rootKey), nullptr));
        }
    } else {
        TileKey rootKey{scheme.id(), 0, 0, 0};
        roots_.push_back(std::make_unique<TileNode>(
            rootKey, scheme.tileToRectangle(rootKey), nullptr));
    }

    createdNodeCount_ = static_cast<int>(roots_.size());
}

TilePlan TileQuadTree::compute(const Camera& camera,
                               const TileScheme& scheme,
                               double viewportWidthPixels,
                               double viewportHeightPixels,
                               int previousZoom) {
    TilePlan plan;
    ensureRoot(scheme);
    for (auto& root : roots_) {
        root->resetFrameState();
    }
    std::vector<TileNode*> renderedNodes;

    Vec3 camPos = camera.position();
    double cameraHeight = camPos.length() - kEarthRadius;
    if (cameraHeight < 1000.0) cameraHeight = 1000.0;
    const int cameraInsideTargetZoom = zoomLevelFromHeight(
        cameraHeight,
        viewportHeightPixels,
        camera.verticalFovRadians(),
        scheme.minZoom(),
        scheme.maxZoom());

    const Cartographic subCamera = Ellipsoid::WGS84().cartesianToCartographic(
        camera.position().normalized() * kEarthRadius);
    const Frustum frustum = camera.frustum(viewportWidthPixels, viewportHeightPixels);

    for (auto& root : roots_) {
        root->traverse(scheme,
                       camera,
                       frustum,
                       viewportWidthPixels,
                       viewportHeightPixels,
                       subCamera.longitude(),
                       subCamera.latitude(),
                       true,
                       cameraInsideTargetZoom,
                       kMaxRenderedNodes,
                       plan.visibleTiles,
                       renderedNodes);
    }

    plan.lodSizePixels = std::clamp(
        openglobusLodSizePixels(camera),
        kOpenGlobusMaxLodPixels,
        kOpenGlobusMinLodPixels);
    plan.minLodSizePixels = kOpenGlobusMinLodPixels;
    plan.maxLodSizePixels = kOpenGlobusMaxLodPixels;

    dedupeAndUpdateZoomStats(plan);
    if (shouldApplyEqualZoom(camera, cameraHeight)) {
        plan.equalZoomApplied = true;
        applyOpenGlobusEqualZoomPass(scheme,
                                      camera,
                                      frustum,
                                      viewportWidthPixels,
                                      viewportHeightPixels,
                                      subCamera.longitude(),
                                      subCamera.latitude(),
                                      plan,
                                      renderedNodes);
        dedupeAndUpdateZoomStats(plan);
        if (previousZoom >= 0 && previousZoom < plan.maxVisibleZoom) {
            plan.equalZoomApplied = true;
        }
    }
    for (const auto& root : roots_) {
        accumulateNodeStats(root.get(), plan);
    }
    accumulateTileGroupStats(plan);
    accumulateNeighborStats(renderedNodes, plan);
    updateTileTransitions(renderedNodes, plan);

    createdNodeCount_ = 0;
    for (const auto& root : roots_) {
        createdNodeCount_ += root ? root->subtreeNodeCount() : 0;
    }
    lastVisitedNodeCount_ = static_cast<int>(renderedNodes.size());
    return plan;
}

} // namespace earth_engine
