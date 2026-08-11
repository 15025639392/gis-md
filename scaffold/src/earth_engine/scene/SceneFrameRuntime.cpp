#include "SceneFrameRuntime.h"

#include "SceneInteractionCoordinator.h"

#include <utility>

namespace earth_engine {

void SceneFrameRuntime::setViewport(
    int widthPixels,
    int heightPixels,
    float dpr) {
    frameState_.viewportWidthPixels = widthPixels;
    frameState_.viewportHeightPixels = heightPixels;
    frameState_.devicePixelRatio = dpr;
}

void SceneFrameRuntime::setSelectorViewOverride(
    std::vector<SelectorView> selectorViews) {
    hasSelectorViewOverride_ = true;
    selectorViewOverride_ = std::move(selectorViews);
}

void SceneFrameRuntime::clearSelectorViewOverride() {
    hasSelectorViewOverride_ = false;
    selectorViewOverride_.clear();
}

SceneFrameUpdateInput SceneFrameRuntime::makeFrameUpdateInput(
    Diagnostics& diagnostics,
    Camera* camera,
    CameraSystem* cameraSystem,
    IPrepareRendererResources* pPrepRenderer,
    SceneTilesetCoordinator& tilesets,
    double deltaSeconds,
    bool hasInteractionFocus,
    Vec3 interactionFocusDirection,
    double interactionFocusTimeSeconds,
    TimeController* timeController,
    SkyGradient* skyGradient) {
    return SceneFrameUpdateInput{
        frameState_,
        diagnostics,
        camera,
        cameraSystem,
        pPrepRenderer,
        tilesets,
        frameId_,
        elapsedTime_,
        deltaSeconds,
        hasSelectorViewOverride_,
        &selectorViewOverride_,
        hasInteractionFocus,
        interactionFocusDirection,
        interactionFocusTimeSeconds,
        timeController,
        skyGradient};
}

SceneInteractionContext SceneFrameRuntime::makeInteractionContext(
    Camera* camera,
    CameraSystem* cameraSystem,
    const Tileset* terrainTileset,
    const std::vector<std::unique_ptr<VectorLayer>>* vectorLayers) const {
    return SceneInteractionContext{
        camera,
        cameraSystem,
        static_cast<double>(frameState_.viewportWidthPixels),
        static_cast<double>(frameState_.viewportHeightPixels),
        terrainTileset,
        vectorLayers,
        elapsedTime_};
}

} // namespace earth_engine
