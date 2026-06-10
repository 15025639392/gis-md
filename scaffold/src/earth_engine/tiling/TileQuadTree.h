#pragma once

#include "TileKey.h"
#include "TilePlan.h"
#include "../core/math/BoundingSphere.h"
#include "../core/math/OrientedBoundingBox.h"
#include "../core/math/Rectangle.h"
#include "../core/math/Vec3.h"

#include <array>
#include <memory>
#include <unordered_set>
#include <vector>

namespace earth_engine {

class Camera;
class Frustum;
class TileScheme;

enum class TileNodeState {
    NotRendering,
    Rendering,
    Walkthrough
};

class TileNode {
public:
    TileNode(TileKey key, Rectangle bounds, TileNode* parent = nullptr);

    const TileKey& key() const { return key_; }
    const Rectangle& bounds() const { return bounds_; }
    const TileNode* parent() const { return parent_; }
    TileNodeState state() const { return state_; }
    TileSelectionState selectionState() const { return selectionState_; }
    TileSelectionState previousSelectionState() const { return previousSelectionState_; }
    bool cameraInside() const { return cameraInside_; }
    int inFrustumMask() const { return inFrustumMask_; }
    TileNodeState previousState() const { return previousState_; }
    double transitionOpacity() const { return transitionOpacity_; }
    double screenSpaceError() const { return screenSpaceError_; }
    bool ancestorMeetsSse() const { return ancestorMeetsSse_; }
    int fadingNodeCount() const { return fadingNodeCount_; }
    double transitionTimestamp() const { return transitionTimestamp_; }
    void animateTransitionOpacity(double elapsedSinceTransition);
    bool isHorizonTangent(const Camera& camera) const;
    /// cesium-native aligned bounding volumes for frustum culling.
    const BoundingSphere& boundingSphere() const { return boundingSphere_; }
    const OrientedBoundingBox& boundingBox() const { return obb_; }
    const Vec3& boundingCenter() const { return boundingSphere_.getCenter(); }
    double boundingRadiusMeters() const { return boundingSphere_.getRadius(); }
    const std::array<Vec3, 4>& cornerPoints() const { return cornerPoints_; }
    bool childrenCreated() const { return children_[0] != nullptr; }
    int subtreeNodeCount() const;

    void resetFrameState();
    void ensureChildren(const TileScheme& scheme);
    void markNeighborBalancedRendering();

    void traverse(const TileScheme& scheme,
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
                  std::vector<TileNode*>& renderedNodes);

    void traverseImpl(const TileScheme& scheme,
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
                  bool parentInsideFrustum);

    void renderToZoom(const TileScheme& scheme,
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
                      std::vector<TileNode*>& renderedNodes);

    const std::array<std::unique_ptr<TileNode>, 4>& children() const {
        return children_;
    }

private:
    bool containsCartographic(double longitudeRad, double latitudeRad) const;
    bool isAltitudeVisible(const Camera& camera) const;
    bool intersectsAny(const std::vector<Rectangle>& rectangles) const;
    bool shouldSubdivide(const Camera& camera,
                         double viewportWidthPixels,
                         double viewportHeightPixels,
                         double cameraHeightMeters,
                         double& outScreenSpaceError) const;
    bool childrenPreviousStateEquals(TileNodeState state) const;
    void markRenderingTransition();
    void markFadingOut();  // parent fading out as children take over
    void traverse(const TileScheme& scheme,
                  const std::vector<Rectangle>& visibleFootprint,
                  const std::unordered_set<TileKey>& forcedTiles,
                  int targetZoom,
                  std::vector<TileKey>& out);

    TileKey key_;
    Rectangle bounds_;
    TileNode* parent_ = nullptr;
    TileNodeState state_ = TileNodeState::NotRendering;
    TileNodeState previousState_ = TileNodeState::NotRendering;
    TileSelectionState selectionState_ = TileSelectionState::NotVisited;
    TileSelectionState previousSelectionState_ = TileSelectionState::NotVisited;
    double screenSpaceError_ = 0.0;
    bool ancestorMeetsSse_ = false;
    bool cameraInside_ = false;
    int inFrustumMask_ = 0;
    double transitionOpacity_ = 1.0;
    int fadingNodeCount_ = 0;
    double transitionTimestamp_ = 0.0;
    bool fadingOut_ = false;  // true = 1→0 fade, false = 0→1 fade
    BoundingSphere boundingSphere_{Vec3::zero(), 0.0};
    OrientedBoundingBox obb_{Vec3::zero(), Vec3::unitX(), Vec3::unitY(), Vec3::unitZ()};
    std::array<Vec3, 4> cornerPoints_{};
    std::array<std::unique_ptr<TileNode>, 4> children_{};
};

class TileQuadTree {
public:
    TileQuadTree() = default;

    TilePlan compute(const Camera& camera,
                     const TileScheme& scheme,
                     double viewportWidthPixels,
                     double viewportHeightPixels,
                     int previousZoom = -1);

    const TileNode* root() const { return roots_.empty() ? nullptr : roots_.front().get(); }
    int createdNodeCount() const { return createdNodeCount_; }
    int lastVisitedNodeCount() const { return lastVisitedNodeCount_; }

private:
    void ensureRoot(const TileScheme& scheme);
    void resetIfSchemeChanged(const TileScheme& scheme);

    std::vector<std::unique_ptr<TileNode>> roots_;
    std::string schemeId_;
    int createdNodeCount_ = 0;
    int lastVisitedNodeCount_ = 0;
    double lastFrameTime_ = 0.0;  // for transition opacity animation
};

} // namespace earth_engine
