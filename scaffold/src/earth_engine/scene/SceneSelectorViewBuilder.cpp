#include "SceneSelectorViewBuilder.h"

#include "Camera.h"
#include "FrameState.h"

namespace earth_engine {

void SceneSelectorViewBuilder::populate(
    FrameState& frameState,
    const SceneSelectorViewBuildInput& input) {
    frameState.selectorViews.clear();
    if (input.hasOverride) {
        if (input.overrideViews) {
            frameState.selectorViews = *input.overrideViews;
        }
        return;
    }

    if (!input.camera) {
        return;
    }

    SelectorView selectorView;
    selectorView.position = input.camera->position();
    selectorView.direction = input.camera->direction();
    const double viewportWidth =
        static_cast<double>(input.viewportWidthPixels);
    const double viewportHeight =
        static_cast<double>(input.viewportHeightPixels);
    selectorView.projectionMatrix =
        input.camera->projectionMatrix(viewportWidth, viewportHeight);
    selectorView.frustum = Frustum::fromViewProjection(
        selectorView.projectionMatrix * input.camera->viewMatrix());
    selectorView.viewportHeightPixels = input.viewportHeightPixels;
    frameState.selectorViews.push_back(selectorView);
}

} // namespace earth_engine
