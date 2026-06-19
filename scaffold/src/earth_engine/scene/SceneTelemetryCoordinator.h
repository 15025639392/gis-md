#pragma once

#include "Diagnostics.h"
#include "EngineTimingScope.h"
#include "PresentationTrace.h"

namespace earth_engine {

struct ScenePresentationTraceInput;

class SceneTelemetryCoordinator {
public:
    const Diagnostics& diagnostics() const { return diagnostics_; }
    Diagnostics& diagnostics() { return diagnostics_; }

    const PresentationTrace& presentationTrace() const {
        return presentationTrace_;
    }

    void recordEngineTiming(EngineTimingScope scope, double elapsedMs);
    void finishEngineFrame(double elapsedMs);
    void replaceRenderDiagnostics(const Diagnostics& diagnostics);
    void updatePresentationTrace(const ScenePresentationTraceInput& input);

private:
    Diagnostics diagnostics_;
    PresentationTrace presentationTrace_;
};

} // namespace earth_engine
