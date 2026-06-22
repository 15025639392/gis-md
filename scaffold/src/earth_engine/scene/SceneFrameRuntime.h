#pragma once

#include "FrameState.h"
#include "SceneFrameUpdateCoordinator.h"
#include "../renderer/RenderCommand.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace earth_engine {

class Camera;
class CameraController;
struct Diagnostics;
class IPrepareRendererResources;
class SceneTilesetCoordinator;
class SkyGradient;
class Tileset;
class TimeController;
class VectorLayer;
struct SceneInteractionContext;

class SceneFrameRuntime {
public:
    FrameState& frameState() { return frameState_; }
    const FrameState& frameState() const { return frameState_; }

    RenderCommandList& renderCommands() { return renderCommands_; }
    const RenderCommandList& renderCommands() const { return renderCommands_; }

    uint64_t& frameId() { return frameId_; }
    uint64_t frameId() const { return frameId_; }

    double& elapsedTime() { return elapsedTime_; }
    double elapsedTime() const { return elapsedTime_; }

    void setViewport(int widthPixels, int heightPixels, float dpr);
    void setSelectorViewOverride(
        std::vector<SelectorView> selectorViews);
    void clearSelectorViewOverride();

    SceneFrameUpdateInput makeFrameUpdateInput(
        Diagnostics& diagnostics,
        Camera* camera,
        CameraController* cameraController,
        IPrepareRendererResources* pPrepRenderer,
        SceneTilesetCoordinator& tilesets,
        double deltaSeconds,
        bool hasInteractionFocus,
        Vec3 interactionFocusDirection,
        double interactionFocusTimeSeconds,
        TimeController* timeController,
        SkyGradient* skyGradient);
    SceneInteractionContext makeInteractionContext(
        Camera* camera,
        CameraController* cameraController,
        const Tileset* terrainTileset,
        const std::vector<std::unique_ptr<VectorLayer>>* vectorLayers) const;
    bool hasSelectorViewOverride() const {
        return hasSelectorViewOverride_;
    }
    const std::vector<SelectorView>& selectorViewOverride() const {
        return selectorViewOverride_;
    }

private:
    FrameState frameState_;
    RenderCommandList renderCommands_;
    uint64_t frameId_ = 0;
    double elapsedTime_ = 0.0;
    bool hasSelectorViewOverride_ = false;
    std::vector<SelectorView> selectorViewOverride_;
};

} // namespace earth_engine
