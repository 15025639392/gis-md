#pragma once

#include "SelectorView.h"

#include <vector>

namespace earth_engine {

class Camera;
struct FrameState;

struct SceneSelectorViewBuildInput {
    const Camera* camera = nullptr;
    int viewportWidthPixels = 0;
    int viewportHeightPixels = 0;
    bool hasOverride = false;
    const std::vector<SelectorView>* overrideViews = nullptr;
};

class SceneSelectorViewBuilder {
public:
    static void populate(FrameState& frameState,
                         const SceneSelectorViewBuildInput& input);
};

} // namespace earth_engine
