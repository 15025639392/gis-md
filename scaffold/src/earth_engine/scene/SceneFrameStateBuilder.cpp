#include "SceneFrameStateBuilder.h"

#include "Camera.h"
#include "SceneSelectorViewBuilder.h"

#include "../core/geodesy/Ellipsoid.h"
#include "../debug/PerfTimer.h"
#include "../environment/SkyGradient.h"
#include "../environment/SunDirection.h"
#include "../environment/TimeController.h"

namespace earth_engine {
namespace {

constexpr double kInteractionFocusTtlSeconds = 2.5;

void updateInteractionFocus(
    FrameState& frameState,
    bool hasInteractionFocus,
    const Vec3& interactionFocusDirection,
    double interactionFocusTimeSeconds) {
    frameState.hasInteractionFocus =
        hasInteractionFocus &&
        interactionFocusTimeSeconds >= 0.0 &&
        frameState.timeSeconds - interactionFocusTimeSeconds <=
            kInteractionFocusTtlSeconds;
    frameState.interactionFocusDirection = frameState.hasInteractionFocus
        ? interactionFocusDirection
        : Vec3::zero();
}

double updateEnvironment(const SceneFrameStateBuildInput& input) {
    if (!input.timeController || !input.skyGradient || !input.camera) {
        return 0.0;
    }

    const double startMs = perf::nowMs();
    FrameState& frameState = input.frameState;
    Vec3 sunDir = SunDirection::compute(input.timeController->julianDate());
    double camAlt = input.camera->getHeight();
    Vec3 localUp =
        Ellipsoid::WGS84().geodeticSurfaceNormal(input.camera->position());
    input.skyGradient->update(sunDir, localUp, camAlt);
    frameState.lightDir = {
        static_cast<float>(sunDir.x()),
        static_cast<float>(sunDir.y()),
        static_cast<float>(sunDir.z())};
    auto& hc = input.skyGradient->horizonColor();
    frameState.clearR = hc[0];
    frameState.clearG = hc[1];
    frameState.clearB = hc[2];
    auto& ac = input.skyGradient->ambientColor();
    frameState.ambient = {ac[0], ac[1], ac[2]};
    return perf::nowMs() - startMs;
}

} // namespace

SceneFrameStateBuildResult SceneFrameStateBuilder::build(
    const SceneFrameStateBuildInput& input) {
    FrameState& frameState = input.frameState;
    frameState.frameId = input.frameId;
    frameState.timeSeconds = input.timeSeconds;
    frameState.deltaSeconds = input.deltaSeconds;
    frameState.camera = input.camera;

    SceneSelectorViewBuilder::populate(
        frameState,
        SceneSelectorViewBuildInput{
            input.camera,
            frameState.viewportWidthPixels,
            frameState.viewportHeightPixels,
            input.hasSelectorViewOverride,
            input.selectorViewOverride});
    updateInteractionFocus(
        frameState,
        input.hasInteractionFocus,
        input.interactionFocusDirection,
        input.interactionFocusTimeSeconds);
    return SceneFrameStateBuildResult{
        updateEnvironment(input)};
}

} // namespace earth_engine
