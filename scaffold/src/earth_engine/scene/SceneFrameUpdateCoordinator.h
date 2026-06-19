#pragma once

#include "FrameState.h"

#include "../core/math/Vec3.h"

#include <cstdint>
#include <vector>

namespace earth_engine {

class Camera;
class CameraController;
struct Diagnostics;
class SceneTilesetCoordinator;
class SkyGradient;
class TimeController;

struct SceneFrameUpdateInput {
    FrameState& frameState;
    Diagnostics& diagnostics;
    Camera* camera = nullptr;
    CameraController* cameraController = nullptr;
    SceneTilesetCoordinator& tilesets;
    uint64_t& frameId;
    double& elapsedTime;
    double deltaSeconds = 0.0;
    bool hasSelectorViewOverride = false;
    const std::vector<FrameState::SelectorView>* selectorViewOverride =
        nullptr;
    bool hasInteractionFocus = false;
    Vec3 interactionFocusDirection = Vec3::zero();
    double interactionFocusTimeSeconds = -1.0;
    TimeController* timeController = nullptr;
    SkyGradient* skyGradient = nullptr;
};

class SceneFrameUpdateCoordinator {
public:
    static void update(const SceneFrameUpdateInput& input);
};

} // namespace earth_engine
