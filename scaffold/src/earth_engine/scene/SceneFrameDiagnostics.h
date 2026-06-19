#pragma once

#include "Diagnostics.h"
#include "EngineTimingScope.h"

namespace earth_engine {

class SceneFrameDiagnostics {
public:
    using EngineTimingScope = earth_engine::EngineTimingScope;

    static void resetPerFrame(Diagnostics& diagnostics);
    static void updateFrameRate(Diagnostics& diagnostics,
                                double deltaSeconds);
    static void recordEngineTiming(Diagnostics& diagnostics,
                                   EngineTimingScope scope,
                                   double elapsedMs);
    static void finishEngineFrame(Diagnostics& diagnostics,
                                  double elapsedMs);
};

} // namespace earth_engine
