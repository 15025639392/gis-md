#include "ScenePickingCoordinator.h"

#include "../layers/VectorLayer.h"
#include "../scene/Camera.h"
#include "SceneTerrainQuery.h"

namespace earth_engine {

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
        (!result.isValid() || vectorResult.distance < result.distance)) {
        result = vectorResult;
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
