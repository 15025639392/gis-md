#pragma once

#include "FrameState.h"
#include "../renderer/RenderCommand.h"

#include <cstdint>
#include <vector>

namespace earth_engine {

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
        std::vector<FrameState::SelectorView> selectorViews);
    void clearSelectorViewOverride();

    bool hasSelectorViewOverride() const {
        return hasSelectorViewOverride_;
    }
    const std::vector<FrameState::SelectorView>& selectorViewOverride() const {
        return selectorViewOverride_;
    }

private:
    FrameState frameState_;
    RenderCommandList renderCommands_;
    uint64_t frameId_ = 0;
    double elapsedTime_ = 0.0;
    bool hasSelectorViewOverride_ = false;
    std::vector<FrameState::SelectorView> selectorViewOverride_;
};

} // namespace earth_engine
