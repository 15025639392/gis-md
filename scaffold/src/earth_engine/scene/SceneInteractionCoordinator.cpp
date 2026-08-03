#include "SceneInteractionCoordinator.h"

#include "SceneTerrainQuery.h"
#include "../camera/CameraController.h"
#include "../tiling/Tileset.h"

namespace earth_engine {

SceneInteractionCoordinator::SceneInteractionCoordinator()
    : inputManager_(std::make_unique<InputManager>()),
      pickingService_(std::make_unique<PickingService>()),
      selectionManager_(std::make_unique<SelectionManager>()) {
    setupInputCallback();
}

void SceneInteractionCoordinator::setFeatureStateChangeCallback(
    FeatureStateChangeCallback callback) {
    selectionManager_->setStateChangeCallback(std::move(callback));
}

void SceneInteractionCoordinator::configureCameraSurfacePicker(
    CameraController& cameraController,
    std::function<SceneInteractionContext()> contextProvider) {
    cameraController.setSurfacePicker(
        [this, contextProvider](float screenX, float screenY, Vec3& outPoint) {
            return pickInteractionFocus(
                contextProvider(),
                screenX,
                screenY,
                outPoint);
        });

    cameraController.setTerrainHeightFunc(
        [contextProvider](const Vec3& ecefPosition)
            -> std::optional<double> {
            return SceneTerrainQuery::sampleHeight(
                contextProvider().terrainTileset,
                ecefPosition);
        });

    // 近场探针:区域批量采样(碰撞钳位主路径,单点 TerrainHeightFunc 退为
    // 无探针回退)+ 数据代次(contentBytesUsed 作变更计数代理,与矢量贴地
    // SceneRenderPipeline 同一惯用法)。
    cameraController.setTerrainAreaSampleFunc(
        [contextProvider](const Vec3& groundEcef,
                          double radiusMeters,
                          const std::vector<glm::dvec2>& offsets,
                          std::vector<CameraController::TerrainSample>& out) {
            SceneTerrainQuery::sampleAreaHeights(
                contextProvider().terrainTileset,
                groundEcef,
                radiusMeters,
                offsets,
                out);
        });
    cameraController.setTerrainRevisionFunc(
        [contextProvider]() -> uint64_t {
            const Tileset* tileset = contextProvider().terrainTileset;
            return tileset
                ? static_cast<uint64_t>(tileset->contentBytesUsed())
                : 0;
        });
}

PickResult SceneInteractionCoordinator::pick(
    const SceneInteractionContext& context,
    float screenX,
    float screenY) const {
    return ScenePickingCoordinator::pick(
        pickingContext(context),
        screenX,
        screenY);
}

bool SceneInteractionCoordinator::pickInteractionFocus(
    const SceneInteractionContext& context,
    float screenX,
    float screenY,
    Vec3& outPoint) const {
    return ScenePickingCoordinator::pickInteractionFocus(
        pickingContext(context),
        screenX,
        screenY,
        outPoint);
}

void SceneInteractionCoordinator::onInputEvent(
    const SceneInteractionContext& context,
    const InputEvent& event) {
    updateInteractionFocus(context, event);
    if (!inputManager_) {
        return;
    }

    activeInputContext_ = &context;
    inputManager_->process(event);
    activeInputContext_ = nullptr;
}

void SceneInteractionCoordinator::onHover(const PickResult& result) {
    if (selectionManager_) {
        selectionManager_->onHover(result);
    }
}

void SceneInteractionCoordinator::onSelect(const PickResult& result) {
    if (selectionManager_) {
        selectionManager_->onSelect(result);
    }
}

void SceneInteractionCoordinator::clearSelection() {
    if (selectionManager_) {
        selectionManager_->clearSelection();
    }
}

ScenePickingContext SceneInteractionCoordinator::pickingContext(
    const SceneInteractionContext& context) const {
    return ScenePickingContext{
        pickingService_.get(),
        context.camera,
        context.viewportWidthPixels,
        context.viewportHeightPixels,
        context.terrainTileset,
        context.vectorLayers};
}

SceneInputCoordinatorContext SceneInteractionCoordinator::inputContext(
    const SceneInteractionContext& context) const {
    return SceneInputCoordinatorContext{
        context.cameraController,
        selectionManager_.get(),
        [this, &context](float screenX, float screenY) {
            return pick(context, screenX, screenY);
        },
        [this, &context](float screenX, float screenY, Vec3& outPoint) {
            return pickInteractionFocus(context, screenX, screenY, outPoint);
        },
        context.elapsedTimeSeconds};
}

void SceneInteractionCoordinator::setupInputCallback() {
    inputManager_->setCallback(
        [this](InputManager::Gesture gesture, const InputEvent& event) {
            if (!activeInputContext_) {
                return;
            }
            SceneInputCoordinator::handleGesture(
                inputContext(*activeInputContext_),
                gesture,
                event);
        });
}

void SceneInteractionCoordinator::updateInteractionFocus(
    const SceneInteractionContext& context,
    const InputEvent& event) {
    SceneInputCoordinator::updateInteractionFocus(
        inputContext(context),
        event,
        focusState_);
}

} // namespace earth_engine
