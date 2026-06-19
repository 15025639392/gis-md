#include "ScenePickingCoordinator.h"

#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../layers/VectorLayer.h"
#include "../scene/Camera.h"
#include "../tiling/Tileset.h"

namespace earth_engine {

std::function<float(double, double)>
ScenePickingCoordinator::makeTerrainSampler(const Tileset* terrainTileset) {
    if (!terrainTileset) {
        return {};
    }
    return [terrainTileset](double lng, double lat) {
        return terrainTileset->sampleHeight(lng, lat);
    };
}

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
        makeTerrainSampler(context.terrainTileset));

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
        makeTerrainSampler(context.terrainTileset));
    if (!result.isValid()) {
        return false;
    }

    outPoint = result.worldPosition;
    return true;
}

double ScenePickingCoordinator::sampleTerrainHeight(
    const Tileset* terrainTileset,
    const Vec3& ecefPosition) {
    if (!terrainTileset) {
        return 0.0;
    }
    const Cartographic c =
        Ellipsoid::WGS84().cartesianToCartographic(ecefPosition);
    return static_cast<double>(
        terrainTileset->sampleHeight(c.longitude(), c.latitude()));
}

CameraController::TerrainHeightFunc
ScenePickingCoordinator::makeTerrainHeightFunc(const Tileset* terrainTileset) {
    if (!terrainTileset) {
        return {};
    }
    return [terrainTileset](const Vec3& ecefPosition) -> double {
        return sampleTerrainHeight(terrainTileset, ecefPosition);
    };
}

} // namespace earth_engine
