#pragma once

#include "FrameState.h"
#include "SceneFrameUpdateCoordinator.h"
#include "../core/resources/SceneFrameResourceArbiter.h"
#include "../renderer/RenderCommand.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace earth_engine {

class Camera;
class CameraSystem;
struct Diagnostics;
class IPrepareRendererResources;
class SceneTilesetCoordinator;
class SkyGradient;
class Tileset;
class TimeController;
class VectorLayer;
class FeatureRenderLayer;
struct SceneInteractionContext;

class SceneFrameRuntime {
public:
    FrameState& frameState() { return frameState_; }
    const FrameState& frameState() const { return frameState_; }

    RenderCommandList& renderCommands() { return renderCommands_; }
    const RenderCommandList& renderCommands() const { return renderCommands_; }

    uint64_t& frameId() { return frameId_; }
    uint64_t frameId() const { return frameId_; }

    SceneFrameResourceArbiter& resourceArbiter() {
        return resourceArbiter_;
    }
    const SceneFrameResourceArbiter& resourceArbiter() const {
        return resourceArbiter_;
    }

    double& elapsedTime() { return elapsedTime_; }
    double elapsedTime() const { return elapsedTime_; }

    void setViewport(int widthPixels, int heightPixels, float dpr);
    void setSelectorViewOverride(
        std::vector<SelectorView> selectorViews);
    void clearSelectorViewOverride();

    SceneFrameUpdateInput makeFrameUpdateInput(
        Diagnostics& diagnostics,
        Camera* camera,
        CameraSystem* cameraSystem,
        IPrepareRendererResources* pPrepRenderer,
        SceneTilesetCoordinator& tilesets,
        bool mvtActive,
        uint32_t mvtSourceCount,
        bool pageStoreActive,
        double deltaSeconds,
        bool hasInteractionFocus,
        Vec3 interactionFocusDirection,
        double interactionFocusTimeSeconds,
        TimeController* timeController,
        SkyGradient* skyGradient);
    SceneInteractionContext makeInteractionContext(
        Camera* camera,
        CameraSystem* cameraSystem,
        const Tileset* terrainTileset,
        const std::vector<std::unique_ptr<VectorLayer>>* vectorLayers,
        const std::vector<std::unique_ptr<FeatureRenderLayer>>*
            featureRenderLayers = nullptr,
        const FrameState* frameState = nullptr) const;
    bool hasSelectorViewOverride() const {
        return hasSelectorViewOverride_;
    }
    const std::vector<SelectorView>& selectorViewOverride() const {
        return selectorViewOverride_;
    }

private:
    FrameState frameState_;
    RenderCommandList renderCommands_;
    SceneFrameResourceArbiter resourceArbiter_;
    uint64_t frameId_ = 0;
    double elapsedTime_ = 0.0;
    bool hasSelectorViewOverride_ = false;
    std::vector<SelectorView> selectorViewOverride_;
};

} // namespace earth_engine
