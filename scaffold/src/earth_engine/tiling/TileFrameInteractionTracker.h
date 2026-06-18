#pragma once

#include "../core/math/Vec3.h"
#include "../scene/FrameState.h"

namespace earth_engine {

struct TileFrameInteractionSnapshot {
    Vec3 cameraPosition = Vec3::zero();
    Vec3 cameraDirection = Vec3::zero();
    double lastInteractionActiveTimeSeconds = -1.0;
    bool cameraMoving = false;
    bool interactionActive = false;
    bool resourceSmoothingActive = false;
};

class TileFrameInteractionTracker {
public:
    static TileFrameInteractionSnapshot evaluate(
        const FrameState& frameState,
        const Vec3& previousCameraPosition,
        double previousInteractionActiveTimeSeconds,
        double smoothingWindowSeconds) {
        TileFrameInteractionSnapshot snapshot;
        snapshot.cameraPosition = frameState.camera->position();
        snapshot.cameraDirection = frameState.camera->direction();
        if (previousCameraPosition.lengthSquared() > 0.0) {
            snapshot.cameraMoving =
                snapshot.cameraPosition.distanceTo(previousCameraPosition) >
                2.0;
        }
        snapshot.interactionActive =
            frameState.hasInteractionFocus || snapshot.cameraMoving;
        snapshot.lastInteractionActiveTimeSeconds =
            previousInteractionActiveTimeSeconds;
        if (snapshot.interactionActive) {
            snapshot.lastInteractionActiveTimeSeconds = frameState.timeSeconds;
        }

        const bool postInteractionSmoothing =
            !snapshot.interactionActive &&
            snapshot.lastInteractionActiveTimeSeconds >= 0.0 &&
            frameState.timeSeconds -
                    snapshot.lastInteractionActiveTimeSeconds <=
                smoothingWindowSeconds;
        snapshot.resourceSmoothingActive =
            snapshot.interactionActive || postInteractionSmoothing;
        return snapshot;
    }
};

} // namespace earth_engine
