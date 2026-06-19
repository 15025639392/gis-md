#include "SceneFrameRuntime.h"

#include <utility>

namespace earth_engine {

void SceneFrameRuntime::setViewport(
    int widthPixels,
    int heightPixels,
    float dpr) {
    frameState_.viewportWidthPixels = widthPixels;
    frameState_.viewportHeightPixels = heightPixels;
    frameState_.devicePixelRatio = dpr;
}

void SceneFrameRuntime::setSelectorViewOverride(
    std::vector<FrameState::SelectorView> selectorViews) {
    hasSelectorViewOverride_ = true;
    selectorViewOverride_ = std::move(selectorViews);
}

void SceneFrameRuntime::clearSelectorViewOverride() {
    hasSelectorViewOverride_ = false;
    selectorViewOverride_.clear();
}

} // namespace earth_engine
