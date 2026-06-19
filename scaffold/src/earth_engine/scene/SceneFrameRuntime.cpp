#include "SceneFrameRuntime.h"

#include "SceneInteractionCoordinator.h"
#include "ScenePresentationTraceBuilder.h"

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
    std::vector<FrameState::SelectorView> selectorViews) {
    hasSelectorViewOverride_ = true;
    selectorViewOverride_ = std::move(selectorViews);
}

void SceneFrameRuntime::clearSelectorViewOverride() {
    hasSelectorViewOverride_ = false;
    selectorViewOverride_.clear();
}

SceneInteractionContext SceneFrameRuntime::makeInteractionContext(
    Camera* camera,
    CameraController* cameraController,
    const Tileset* terrainTileset,
    const std::vector<std::unique_ptr<VectorLayer>>* vectorLayers) const {
    return SceneInteractionContext{
        camera,
        cameraController,
        static_cast<double>(frameState_.viewportWidthPixels),
        static_cast<double>(frameState_.viewportHeightPixels),
        terrainTileset,
        vectorLayers,
        elapsedTime_};
}

ScenePresentationTraceInput SceneFrameRuntime::makePresentationTraceInput(
    const Tileset* terrainTileset,
    const std::vector<std::unique_ptr<Tileset>>& additionalTilesets) const {
    return ScenePresentationTraceInput{
        frameState_,
        terrainTileset,
        additionalTilesets,
        renderCommands_};
}

} // namespace earth_engine
