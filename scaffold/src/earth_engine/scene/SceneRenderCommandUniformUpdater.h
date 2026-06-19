#pragma once

#include "FrameState.h"
#include "../renderer/RenderCommand.h"

namespace earth_engine {

class SceneRenderCommandUniformUpdater {
public:
    static void apply(const FrameState& frameState,
                      RenderCommandList& commands);
};

} // namespace earth_engine
