#pragma once

#include "Diagnostics.h"
#include "EngineTimingScope.h"
#include "PresentationTrace.h"
#include "../renderer/RenderCommand.h"

#include <memory>
#include <vector>

namespace earth_engine {

struct FrameState;
class Tileset;

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
    void updatePresentationTrace(
        const FrameState& frameState,
        const Tileset* terrainTileset,
        const std::vector<std::unique_ptr<Tileset>>& additionalTilesets,
        const RenderCommandList& renderCommands);

private:
    Diagnostics diagnostics_;
    PresentationTrace presentationTrace_;
};

} // namespace earth_engine
