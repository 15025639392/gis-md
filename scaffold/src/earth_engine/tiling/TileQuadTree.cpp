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
#include <chrono>
#include <cmath>
#include <functional>
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
constexpr double kCesiumTerrainMapQuality = 0.25;
constexpr double kCesiumTerrainMapWidth = 65.0;
constexpr double kCesiumNativeSpaceAltitudeMeters = 20000000.0;
constexpr double kCesiumNativeMaximumSse = 12.0;
constexpr double kCesiumNativeHighAltitudeMaximumSse = 9.0;
constexpr double kCesiumNativeSpaceMaximumSse = 10.0;
constexpr double kCesiumNativeLowAltitudeMaximumSse = 6.0;
constexpr double kCesiumNativeMidAltitudeMaximumSse = 8.0;
constexpr double kHighAltitudeMinZoomMeters = 1000000.0;
constexpr double kSpaceViewMinZoomMeters = 8000000.0;
constexpr int kHighAltitudeMinRenderableZoom = 3;
constexpr int kSpaceViewMinRenderableZoom = 3;
constexpr size_t kHighAltitudeMaxRenderedNodes = 384;
constexpr size_t kSpaceViewMaxRenderedNodes = 256;

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

BoundingSphere boundingSphereFor(const Rectangle& bounds) {
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
    return BoundingSphere(center, radius);
}

OrientedBoundingBox obbFromCorners(const Vec3& center,
                                    const std::array<Vec3, 4>& corners) {
    // ENU basis at the tile center
    Vec3 up = center.normalized();
    Vec3 east = Vec3::unitZ().cross(up).normalized();
    Vec3 north = up.cross(east).normalized();

    double hEast = 0.0, hNorth = 0.0, hUp = 0.0;
    for (const Vec3& c : corners) {
        Vec3 d = c - center;
        hEast  = std::max(hEast,  std::abs(d.dot(east)));
        hNorth = std::max(hNorth, std::abs(d.dot(north)));
        hUp    = std::max(hUp,    std::abs(d.dot(up)));
    }
    hEast  = std::max(hEast, 1.0);
    hNorth = std::max(hNorth, 1.0);
    hUp    = std::max(hUp,   1.0);

    return OrientedBoundingBox(center,
                                east * hEast,
                                north * hNorth,
                                up * hUp);
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
    if (node->selectionState() != TileSelectionState::NotVisited) {
        plan.selectionRecords.push_back(TileSelectionRecord{
            node->key(),
            node->selectionState(),
            node->previousSelectionState(),
            node->screenSpaceError(),
            node->cameraInside(),
            node->inFrustumMask() != 0,
            node->ancestorMeetsSse()
        });
    }
    switch (node->selectionState()) {
        case TileSelectionState::Rendered:
            ++plan.selectionRenderedCount;
            break;
        case TileSelectionState::Refined:
            ++plan.selectionRefinedCount;
            break;
        case TileSelectionState::Kicked:
            ++plan.selectionKickedCount;
            break;
        case TileSelectionState::NotVisited:
            break;
    }
    if (node->ancestorMeetsSse()) {
        ++plan.selectionAncestorMeetsSseCount;
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

double cesiumTerrainGeometricError(const Rectangle& bounds) {
    // cesium-native LayerJsonTerrainLoader.cpp:267-272
    //   childTile.setGeometricError(
    //       8.0 * calcQuadtreeMaxGeometricError(ellipsoid) *
    //       childGlobeRectangle.computeWidth());
    // The ×8.0 multiplier ensures every level's geometric error halves
    // from the previous level (since tile width halves each LOD).
    const double maxGeometricErrorPerRadian =
        kEarthRadius * kCesiumTerrainMapQuality / kCesiumTerrainMapWidth;
    return 8.0 * maxGeometricErrorPerRadian * bounds.width();
}

bool shouldApplyEqualZoom(const Camera& camera, double cameraHeightMeters) {
    return cameraSlope(camera) > kOpenGlobusMinEqualZoomCameraSlope &&
           cameraHeightMeters < kOpenGlobusMaxEqualZoomAltitudeMeters &&
           cameraHeightMeters > kOpenGlobusMinEqualZoomAltitudeMeters;
}

bool shouldApplyEqualZoom(const Camera& camera,
                          double cameraHeightMeters,
                          size_t maxRenderedTiles) {
    return maxRenderedTiles >= kMaxRenderedNodes &&
           shouldApplyEqualZoom(camera, cameraHeightMeters);
}

size_t maxRenderedTilesForHeight(double cameraHeightMeters) {
    if (cameraHeightMeters > kSpaceViewMinZoomMeters) {
        return kSpaceViewMaxRenderedNodes;
    }
    if (cameraHeightMeters > kHighAltitudeMinZoomMeters) {
        return kHighAltitudeMaxRenderedNodes;
    }
    return kMaxRenderedNodes;
}

int minimumRenderableZoom(double cameraHeightMeters) {
    if (cameraHeightMeters > kSpaceViewMinZoomMeters) {
        return kSpaceViewMinRenderableZoom;
    }
    if (cameraHeightMeters > kHighAltitudeMinZoomMeters) {
        return kHighAltitudeMinRenderableZoom;
    }
    return kAlwaysSubdivideUntilZoom;
}

double maximumScreenSpaceError(const Camera& camera, double cameraHeightMeters) {
    double maximumSse = kCesiumNativeMaximumSse;
    if (cameraHeightMeters < 50000.0) {
        maximumSse = kCesiumNativeLowAltitudeMaximumSse;
    } else if (cameraHeightMeters < 250000.0) {
        maximumSse = kCesiumNativeMidAltitudeMaximumSse;
    } else if (cameraHeightMeters < kCesiumNativeSpaceAltitudeMeters) {
        maximumSse = kCesiumNativeHighAltitudeMaximumSse;
    } else {
        maximumSse = kCesiumNativeSpaceMaximumSse;
    }

    // Very oblique views can otherwise refine a long horizon strip too deeply.
    // Cesium's full selector has fog/dynamic SSE and loading pressure gates;
    // this compact version keeps the same intent with a slope-based relaxation.
    const double slope = cameraSlope(camera);
    if (slope < 0.45) {
        maximumSse += 4.0;
    } else if (slope < 0.65) {
        maximumSse += 2.0;
    }
    return maximumSse;
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

void addNeighborBalancedChildren(const TileScheme& scheme,
                                 TileNode& coarse,
                                 std::vector<TileKey>& out,
                                 std::vector<TileNode*>& renderedNodes,
                                 std::unordered_set<TileKey>& emitted,
                                 TilePlan& plan) {
    if (coarse.key().z >= scheme.maxZoom()) return;

    coarse.ensureChildren(scheme);
    for (const auto& child : coarse.children()) {
        if (!child) continue;
        const TileKey& childKey = child->key();
        if (!emitted.insert(childKey).second) continue;

        child->markNeighborBalancedRendering();
        out.push_back(childKey);
        renderedNodes.push_back(child.get());
        ++plan.neighborBalancedTileCount;
    }
}

void applyNeighborBalancePass(const TileScheme& scheme,
                              TilePlan& plan,
                              std::vector<TileNode*>& renderedNodes) {
    if (renderedNodes.size() < 2) return;

    std::unordered_set<TileKey> emitted;
    emitted.reserve(renderedNodes.size() * 2);
    for (const TileNode* node : renderedNodes) {
        if (node) emitted.insert(node->key());
    }

    std::vector<TileNode*> initialNodes = renderedNodes;
    for (size_t i = 0; i < initialNodes.size(); ++i) {
        TileNode* a = initialNodes[i];
        if (!a) continue;
        for (size_t j = i + 1; j < initialNodes.size(); ++j) {
            TileNode* b = initialNodes[j];
            if (!b || !haveCommonSide(*a, *b)) continue;

            const int zoomDelta = a->key().z - b->key().z;
            if (zoomDelta > 1) {
                addNeighborBalancedChildren(
                    scheme, *b, plan.visibleTiles, renderedNodes, emitted, plan);
            } else if (zoomDelta < -1) {
                addNeighborBalancedChildren(
                    scheme, *a, plan.visibleTiles, renderedNodes, emitted, plan);
            }
        }
    }
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
                                  double cameraHeightMeters,
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
                           cameraHeightMeters,
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
    boundingSphere_ = boundingSphereFor(bounds_);
    cornerPoints_ = tileCornerPoints(bounds_);
    obb_ = obbFromCorners(boundingSphere_.getCenter(), cornerPoints_);
}

void TileNode::resetFrameState() {
    previousState_ = state_;
    previousSelectionState_ = selectionState_;
    state_ = TileNodeState::NotRendering;
    selectionState_ = TileSelectionState::NotVisited;
    screenSpaceError_ = 0.0;
    ancestorMeetsSse_ = false;
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
    return boundingSphere_.getCenter().normalized().dot(-camera.direction()) < kOpenGlobusHorizonTangent;
}

bool TileNode::shouldSubdivide(const Camera& camera,
                               double viewportWidthPixels,
                               double viewportHeightPixels,
                               double cameraHeightMeters,
                               double& outScreenSpaceError) const {
    (void)viewportWidthPixels;

    // cesium-native terrain alignment:
    // geometricError = calcQuadtreeMaxGeometricError(ellipsoid) * rectangleWidth.
    const double geometricError = cesiumTerrainGeometricError(bounds_);

    // For globe surface tiles, a bounding sphere may contain the camera even
    // when the actual closest rendered surface is roughly cameraHeight away.
    // Using height as a lower bound keeps the camera branch from refining to
    // max zoom solely because the eye is inside a coarse bounding sphere.
    const double sphereDistance = std::max(
        0.0,
        camera.position().distanceTo(boundingSphere_.getCenter()) -
            boundingSphere_.getRadius());
    const double distance = std::max(
        1.0,
        std::max(sphereDistance, cameraHeightMeters));

    outScreenSpaceError =
        geometricError * viewportHeightPixels /
        (distance * 2.0 * std::tan(camera.verticalFovRadians() * 0.5));

    return outScreenSpaceError >= maximumScreenSpaceError(camera, cameraHeightMeters);
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
        fadingOut_ = false;
        return;
    }

    if (previousState_ == TileNodeState::Rendering) {
        // Node was rendering last frame and continues to render — no fade.
        transitionOpacity_ = 1.0;
        fadingNodeCount_ = 0;
        fadingOut_ = false;
        return;
    }

    // Fresh node (was NotRendering): fade in from 0→1 over ~0.3s.
    fadingOut_ = false;
    transitionOpacity_ = 0.0;
    transitionTimestamp_ = 0.3;
    fadingNodeCount_ = 0;
    if (parent_ && parent_->previousState_ == TileNodeState::Rendering) {
        fadingNodeCount_ = 1;
    } else if (childrenPreviousStateEquals(TileNodeState::Rendering)) {
        fadingNodeCount_ = 4;
    }
}

void TileNode::markNeighborBalancedRendering() {
    state_ = TileNodeState::Rendering;
    selectionState_ = TileSelectionState::Rendered;
    markRenderingTransition();
}

void TileNode::markFadingOut() {
    if (key_.z < 3) return;
    // Parent fading out as children take over (cesium-native tilesFadingOut).
    fadingOut_ = true;
    transitionOpacity_ = 1.0;   // start fully visible
    transitionTimestamp_ = 0.3; // 0.3s to fade to 0
}

void TileNode::animateTransitionOpacity(double dt) {
    if (transitionTimestamp_ <= 0.0 || dt <= 0.0) return;

    transitionTimestamp_ = std::max(0.0, transitionTimestamp_ - dt);
    const double t = 1.0 - transitionTimestamp_ / 0.3;
    // Cubic ease-out
    const double eased = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
    if (fadingOut_) {
        transitionOpacity_ = std::clamp(1.0 - eased, 0.0, 1.0);  // 1→0
    } else {
        transitionOpacity_ = std::clamp(eased, 0.0, 1.0);         // 0→1
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
                        double cameraHeightMeters,
                        size_t maxRenderedTiles,
                        std::vector<TileKey>& out,
                        std::vector<TileNode*>& renderedNodes) {
    // Propagated: parent was INSIDE the frustum → this node and all
    // descendants are also inside (cesium-native CullingVolume optimization).
    constexpr bool kParentInside = false; // root-caller never passes this
    return traverseImpl(scheme, camera, frustum,
                        viewportWidthPixels, viewportHeightPixels,
                        cameraLongitudeRad, cameraLatitudeRad,
                        parentCameraInside, cameraHeightMeters,
                        maxRenderedTiles, out, renderedNodes,
                        kParentInside);
}

void TileNode::traverseImpl(const TileScheme& scheme,
                        const Camera& camera,
                        const Frustum& frustum,
                        double viewportWidthPixels,
                        double viewportHeightPixels,
                        double cameraLongitudeRad,
                        double cameraLatitudeRad,
                        bool parentCameraInside,
                        double cameraHeightMeters,
                        size_t maxRenderedTiles,
                        std::vector<TileKey>& out,
                        std::vector<TileNode*>& renderedNodes,
                        bool parentInsideFrustum) {
    if (out.size() >= maxRenderedTiles) {
        return;
    }

    cameraInside_ = parentCameraInside &&
                    containsCartographic(cameraLongitudeRad, cameraLatitudeRad);

    // cesium-native CullingVolume: if parent was fully inside the frustum,
    // skip all plane tests for this subtree.
    CullingResult vis = CullingResult::Intersecting;
    if (parentInsideFrustum) {
        inFrustumMask_ = 1;
    } else {
        vis = frustum.computeVisibility(boundingSphere_);
        if (vis != CullingResult::Outside &&
            frustum.intersectsOBB(obb_)) {
            inFrustumMask_ = 1;
        }
    }

    const bool visible = inFrustumMask_ || cameraInside_ || key_.z < 3;
    const bool altVisible = isAltitudeVisible(camera);
    if (!visible) {
        state_ = TileNodeState::NotRendering;
        return;
    }

    const bool canSubdivide = key_.z < scheme.maxZoom();
    const bool mustSubdivide = key_.z < std::max(
        scheme.minZoom(),
        minimumRenderableZoom(cameraHeightMeters));
    double sse = 0.0;
    const bool refineForSse = canSubdivide && shouldSubdivide(
        camera,
        viewportWidthPixels,
        viewportHeightPixels,
        cameraHeightMeters,
        sse);
    screenSpaceError_ = sse;
    const bool refine = mustSubdivide || refineForSse;
    if (!altVisible && !cameraInside_) {
        state_ = TileNodeState::NotRendering;
        selectionState_ = TileSelectionState::NotVisited;
        return;
    }

    if (!refine || !canSubdivide) {
        state_ = TileNodeState::Rendering;
        selectionState_ = TileSelectionState::Rendered;
        markRenderingTransition();
        out.push_back(key_);
        renderedNodes.push_back(this);
        return;
    }

    state_ = TileNodeState::Walkthrough;
    selectionState_ = TileSelectionState::Refined;
    // cesium-native tilesFadingOut: if this node was rendering last frame
    // and now subdivides to children, keep rendering the parent while it
    // fades out over ~0.3s (cross-fade with children fading in).
    if (previousState_ == TileNodeState::Rendering) {
        ancestorMeetsSse_ = !refineForSse;
        markFadingOut();
        out.push_back(key_);
        renderedNodes.push_back(this);
    }
    bool childInside = (vis == CullingResult::Inside) || parentInsideFrustum;
    ensureChildren(scheme);
    for (auto& child : children_) {
        child->traverseImpl(scheme,
                        camera,
                        frustum,
                        viewportWidthPixels,
                        viewportHeightPixels,
                        cameraLongitudeRad,
                        cameraLatitudeRad,
                        cameraInside_,
                        cameraHeightMeters,
                        maxRenderedTiles,
                        out,
                        renderedNodes,
                        childInside);
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
                            double cameraHeightMeters,
                            int targetZoom,
                            bool stopAtHorizon,
                            size_t maxRenderedTiles,
                            std::vector<TileKey>& out,
                            std::vector<TileNode*>& renderedNodes) {
    if (out.size() >= maxRenderedTiles) return;
    cameraInside_ = parentCameraInside &&
                    containsCartographic(cameraLongitudeRad, cameraLatitudeRad);
    // cesium-native CullingVolume optimization
    CullingResult vis = frustum.computeVisibility(boundingSphere_);
    if (vis != CullingResult::Outside && frustum.intersectsOBB(obb_)) {
        inFrustumMask_ = 1;
    }
    const bool visible = inFrustumMask_ || cameraInside_ || key_.z < 3;
    if (!visible) {
        state_ = TileNodeState::NotRendering;
        selectionState_ = TileSelectionState::NotVisited;
        return;
    }
    if (stopAtHorizon && !isAltitudeVisible(camera) && !cameraInside_) {
        state_ = TileNodeState::NotRendering;
        selectionState_ = TileSelectionState::NotVisited;
        return;
    }

    double sse = 0.0;
    (void)shouldSubdivide(
        camera,
        viewportWidthPixels,
        viewportHeightPixels,
        cameraHeightMeters,
        sse);
    screenSpaceError_ = sse;

    if (key_.z >= targetZoom || key_.z >= scheme.maxZoom()) {
        state_ = TileNodeState::Rendering;
        selectionState_ = TileSelectionState::Rendered;
        markRenderingTransition();
        out.push_back(key_);
        renderedNodes.push_back(this);
        return;
    }

    bool childInside = (vis == CullingResult::Inside);
    state_ = TileNodeState::Walkthrough;
    selectionState_ = TileSelectionState::Refined;
    if (previousState_ == TileNodeState::Rendering) {
        ancestorMeetsSse_ = true;
        markFadingOut();
        out.push_back(key_);
        renderedNodes.push_back(this);
    }
    ensureChildren(scheme);
    for (auto& child : children_) {
        if (childInside) child->inFrustumMask_ = 1;
        child->renderToZoom(scheme,
                            camera,
                            frustum,
                            viewportWidthPixels,
                            viewportHeightPixels,
                            cameraLongitudeRad,
                            cameraLatitudeRad,
                            cameraInside_,
                            cameraHeightMeters,
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
    } else if (scheme.id() == "Geographic-TMS") {
        for (int rootX = 0; rootX < 2; ++rootX) {
            TileKey rootKey{scheme.id(), 0, rootX, 0};
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

    const Cartographic cameraCart = Ellipsoid::WGS84().cartesianToCartographic(
        camera.position());
    double cameraHeight = cameraCart.height();
    if (cameraHeight < 1.0) cameraHeight = 1.0;

    const Cartographic subCamera = Cartographic::fromRadians(
        cameraCart.longitude(),
        cameraCart.latitude(),
        0.0);
    const Frustum frustum = camera.frustum(viewportWidthPixels, viewportHeightPixels);
    const size_t maxRenderedTiles = maxRenderedTilesForHeight(cameraHeight);

    for (auto& root : roots_) {
        root->traverse(scheme,
                       camera,
                       frustum,
                       viewportWidthPixels,
                       viewportHeightPixels,
                       subCamera.longitude(),
                       subCamera.latitude(),
                       true,
                       cameraHeight,
                       maxRenderedTiles,
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
    if (shouldApplyEqualZoom(camera, cameraHeight, maxRenderedTiles)) {
        plan.equalZoomApplied = true;
        applyOpenGlobusEqualZoomPass(scheme,
                                      camera,
                                      frustum,
                                      viewportWidthPixels,
                                      viewportHeightPixels,
                                      subCamera.longitude(),
                                      subCamera.latitude(),
                                      cameraHeight,
                                      plan,
                                      renderedNodes);
        dedupeAndUpdateZoomStats(plan);
        if (previousZoom >= 0 && previousZoom < plan.maxVisibleZoom) {
            plan.equalZoomApplied = true;
        }
    }

    applyNeighborBalancePass(scheme, plan, renderedNodes);
    dedupeAndUpdateZoomStats(plan);
    for (const auto& root : roots_) {
        accumulateNodeStats(root.get(), plan);
    }

    // Animate transition opacity for newly-rendered nodes (OpenGlobus fade-in).
    {
        auto now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        double dt = (lastFrameTime_ > 0.0) ? now - lastFrameTime_ : 1.0 / 60.0;
        lastFrameTime_ = now;

        // Animate rendered nodes
        for (TileNode* node : renderedNodes) {
            node->animateTransitionOpacity(dt);
        }
        // Also animate not-rendering nodes (they might be fading out)
        std::function<void(TileNode*)> animateAll = [&](TileNode* n) {
            if (!n) return;
            n->animateTransitionOpacity(dt);
            for (const auto& child : n->children()) {
                animateAll(child.get());
            }
        };
        for (auto& root : roots_) {
            animateAll(root.get());
        }
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
