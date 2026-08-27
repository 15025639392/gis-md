#include "ScenePickingCoordinator.h"

#include "../layers/FeatureRenderLayer.h"
#include "../layers/VectorLayer.h"
#include "../scene/Camera.h"
#include "../scene/FrameState.h"
#include "SceneTerrainQuery.h"

#include <string>

namespace earth_engine {

namespace {

constexpr float kFeaturePickTolerancePixels = 12.0f;

PickResult::FeaturePart toFeaturePart(FeaturePickResult::Part part) {
    switch (part) {
        case FeaturePickResult::Part::Vertex:
            return PickResult::FeaturePart::Vertex;
        case FeaturePickResult::Part::Edge:
            return PickResult::FeaturePart::Edge;
        case FeaturePickResult::Part::Fill:
            return PickResult::FeaturePart::Fill;
        case FeaturePickResult::Part::None:
        default:
            return PickResult::FeaturePart::None;
    }
}

bool isCloser(const PickResult& candidate, const PickResult& current) {
    if (!candidate.isValid()) return false;
    if (!current.isValid()) return true;
    if (candidate.hitType == PickResult::HitType::VectorFeature &&
        current.hitType != PickResult::HitType::VectorFeature) {
        // CPU screen-space feature picking has a pixel tolerance, so its
        // representative point need not lie exactly on the center pick ray.
        // A small surface slack lets coplanar/ground-clamped features beat the
        // terrain fallback without accepting objects clearly behind terrain.
        constexpr double kSurfaceDepthSlackMeters = 10.0;
        return candidate.distance <=
               current.distance + kSurfaceDepthSlackMeters;
    }
    // Keep the first layer/feature on an exact tie. Scene layer order is the
    // stable tie-break and avoids frame-to-frame selection churn.
    return candidate.distance < current.distance;
}

} // namespace

PickResult ScenePickingCoordinator::pick(
    const ScenePickingContext& context,
    float screenX,
    float screenY) {
    if (!context.pickingService || !context.camera) {
        return PickResult{};
    }

    PickResult result = context.pickingService->pickTerrain(
        screenX,
        screenY,
        *context.camera,
        context.viewportWidthPixels,
        context.viewportHeightPixels,
        SceneTerrainQuery::makeLngLatHeightSampler(context.terrainTileset));

    std::vector<const VectorLayer*> layerPtrs;
    if (context.vectorLayers) {
        layerPtrs.reserve(context.vectorLayers->size());
        for (const auto& layer : *context.vectorLayers) {
            layerPtrs.push_back(layer.get());
        }
    }

    const PickResult vectorResult = context.pickingService->pick(
        screenX,
        screenY,
        *context.camera,
        context.viewportWidthPixels,
        context.viewportHeightPixels,
        layerPtrs);

    if (vectorResult.hitType == PickResult::HitType::VectorFeature &&
        isCloser(vectorResult, result)) {
        result = vectorResult;
    }

    // FeatureRenderLayer owns the FeatureStore path. Reuse its established
    // screen-space query (including ClampToGround and Vertex > Edge > Fill)
    // instead of duplicating geometry tests in the scene coordinator.
    if (context.featureRenderLayers) {
        FrameState pickFrame = context.frameState
                                   ? *context.frameState
                                   : FrameState{};
        pickFrame.camera = context.camera;
        pickFrame.viewportWidthPixels =
            static_cast<int>(context.viewportWidthPixels);
        pickFrame.viewportHeightPixels =
            static_cast<int>(context.viewportHeightPixels);

        for (const auto& layer : *context.featureRenderLayers) {
            if (!layer || !layer->visible()) continue;
            const FeaturePickResult featureResult = layer->pick(
                pickFrame, screenX, screenY, kFeaturePickTolerancePixels);
            if (!featureResult.isValid()) continue;

            PickResult candidate;
            candidate.screenX = screenX;
            candidate.screenY = screenY;
            candidate.hitType = PickResult::HitType::VectorFeature;
            candidate.sourceKind =
                PickResult::FeatureSourceKind::FeatureRenderLayer;
            candidate.featurePart = toFeaturePart(featureResult.part);
            candidate.layerId = layer->id();
            candidate.featureNumericId = featureResult.featureId;
            candidate.featureId = std::to_string(featureResult.featureId);
            candidate.ringIndex = featureResult.ringIndex;
            candidate.vertexIndex = featureResult.vertexIndex;
            candidate.screenDistancePixels = featureResult.distancePx;
            candidate.cartographic = featureResult.renderedPosition;
            candidate.worldPosition = featureResult.worldPosition;
            candidate.distance = featureResult.distanceMeters;
            if (candidate.distance <= 0.0 && pickFrame.camera) {
                candidate.distance = candidate.worldPosition.distanceTo(
                    pickFrame.camera->position());
            }
            if (const Feature* feature =
                    layer->store().getFeature(featureResult.featureId)) {
                candidate.sourceFeatureId = feature->sourceId;
            }

            if (isCloser(candidate, result)) {
                result = std::move(candidate);
            }
        }
    }

    return result;
}

bool ScenePickingCoordinator::pickInteractionFocus(
    const ScenePickingContext& context,
    float screenX,
    float screenY,
    Vec3& outPoint) {
    if (!context.pickingService || !context.camera) {
        return false;
    }

    const PickResult result = context.pickingService->pickTerrain(
        screenX,
        screenY,
        *context.camera,
        context.viewportWidthPixels,
        context.viewportHeightPixels,
        SceneTerrainQuery::makeLngLatHeightSampler(context.terrainTileset));
    if (!result.isValid()) {
        return false;
    }

    outPoint = result.worldPosition;
    return true;
}

} // namespace earth_engine
