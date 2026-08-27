#pragma once

#include "FrameState.h"

#include "../core/math/Vec3.h"

#include <cstdint>
#include <vector>

namespace earth_engine {

class Camera;
class CameraSystem;
struct Diagnostics;
class IPrepareRendererResources;
class SceneTilesetCoordinator;
class SceneFrameResourceArbiter;
class SkyGradient;
class TimeController;

struct SceneFrameUpdateInput {
    FrameState& frameState;
    Diagnostics& diagnostics;
    Camera* camera = nullptr;
    CameraSystem* cameraSystem = nullptr;
    IPrepareRendererResources* pPrepRenderer = nullptr;
    SceneTilesetCoordinator& tilesets;
    SceneFrameResourceArbiter& resourceArbiter;
    // Producers that run after SceneFrameUpdateCoordinator (MVT and
    // PageStore) must be known before allocations are sealed.
    bool mvtActive = false;
    // MVT has one shared producer budget but may contain several independent
    // source instances.  The count lets the bootstrap demand cover the
    // initial fan-out so a later source is not starved by the first source's
    // request in the same frame.
    uint32_t mvtSourceCount = 0;
    bool pageStoreActive = false;
    uint64_t& frameId;
    double& elapsedTime;
    double deltaSeconds = 0.0;
    bool hasSelectorViewOverride = false;
    const std::vector<SelectorView>* selectorViewOverride =
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
