#include "SceneInteractionCoordinator.h"

#include "SceneTerrainQuery.h"
#include "../camera/CameraSystem.h"
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
    CameraSystem& cameraSystem,
    std::function<SceneInteractionContext()> contextProvider) {
    cameraSystem.setSurfacePicker(
        [this, contextProvider](float screenX, float screenY, Vec3& outPoint) {
            return pickInteractionFocus(
                contextProvider(),
                screenX,
                screenY,
                outPoint);
        });

    cameraSystem.setTerrainHeightFunc(
        [contextProvider](const Vec3& ecefPosition)
            -> std::optional<double> {
            return SceneTerrainQuery::sampleHeight(
                contextProvider().terrainTileset,
                ecefPosition);
        });

    // 近场探针:区域批量采样(碰撞钳位主路径,单点 TerrainHeightFunc 退为
    // 无探针回退)+ 数据代次(heightmap 强代次,与矢量贴地
    // SceneRenderPipeline 同一信号源;替代 contentBytesUsed 弱代理)。
    cameraSystem.setTerrainAreaSampleFunc(
        [contextProvider](const Vec3& groundEcef,
                          double radiusMeters,
                          const std::vector<glm::dvec2>& offsets,
                          std::vector<CameraSystem::TerrainSample>& out) {
            SceneTerrainQuery::sampleAreaHeights(
                contextProvider().terrainTileset,
                groundEcef,
                radiusMeters,
                offsets,
                out);
        });
    cameraSystem.setTerrainRevisionFunc(
        [contextProvider]() -> uint64_t {
            return contextProvider().terrainTileset
                ? TerrainHeightService::heightmapGeneration()
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
        context.cameraSystem,
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
