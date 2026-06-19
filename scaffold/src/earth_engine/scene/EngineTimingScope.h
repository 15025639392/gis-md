#pragma once

namespace earth_engine {

enum class EngineTimingScope {
    BeginFrame,
    SceneUpdate,
    SceneRender,
    EndFrame
};

} // namespace earth_engine
