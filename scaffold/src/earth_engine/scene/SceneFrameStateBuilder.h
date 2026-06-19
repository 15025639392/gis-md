#pragma once

#include "FrameState.h"

#include "../core/math/Vec3.h"

#include <cstdint>
#include <vector>

namespace earth_engine {

class Camera;
class SkyGradient;
class TimeController;

struct SceneFrameStateBuildInput {
    FrameState& frameState;
    const Camera* camera = nullptr;
    uint64_t frameId = 0;
    double timeSeconds = 0.0;
    double deltaSeconds = 0.0;
    bool hasSelectorViewOverride = false;
    const std::vector<SelectorView>* selectorViewOverride =
        nullptr;
    bool hasInteractionFocus = false;
    Vec3 interactionFocusDirection = Vec3::zero();
    double interactionFocusTimeSeconds = -1.0;
    const TimeController* timeController = nullptr;
    SkyGradient* skyGradient = nullptr;
};

struct SceneFrameStateBuildResult {
    double environmentUpdateMs = 0.0;
};

class SceneFrameStateBuilder {
public:
    static SceneFrameStateBuildResult build(
        const SceneFrameStateBuildInput& input);
};

} // namespace earth_engine
