#pragma once

#include "Diagnostics.h"
#include "FrameState.h"
#include "../renderer/RenderCommand.h"

#include <functional>
#include <memory>
#include <vector>

namespace earth_engine {

class AtmosphereBackgroundPass;
class Renderer;
class SkyBox;
class SkyGradient;
class Tileset;
class VectorLayer;

/// Lightweight render pass coordinator for Scene's main color pipeline.
///
/// Scene owns long-lived state and updates it; this class turns the current
/// frame state into ordered render commands, validates them, aggregates render
/// diagnostics, and submits the frame.
class SceneRenderPipeline {
public:
    struct Result {
        Diagnostics diagnostics;
    };

    struct Context {
        FrameState& frameState;
        Diagnostics diagnostics;
        Renderer& renderer;
        RenderCommandList& commands;
        SkyBox* skyBox = nullptr;
        AtmosphereBackgroundPass* atmospherePass = nullptr;
        SkyGradient* skyGradient = nullptr;
        Tileset* terrainTileset = nullptr;
        const std::vector<std::unique_ptr<Tileset>>& additionalTilesets;
        std::vector<std::unique_ptr<VectorLayer>>& vectorLayers;
        std::function<void()> beforeSubmit;
    };

    Result render(Context context);

private:
    void reserveCommands(Context& context) const;
    void buildSkyCommands(Context& context, double& skyMs) const;
    void buildAtmosphereCommands(Context& context, double& atmosphereMs) const;
    void buildLayerCommands(Context& context,
                            double& layerCommandsMs,
                            double& vectorCommandsMs) const;
    void applyMvpUniforms(Context& context, double& mvpUniformsMs) const;
    void sortAndValidate(Context& context,
                         double& sortMs,
                         double& surfaceDiagnosticsMs,
                         double& validateMs) const;
    void aggregateDiagnostics(Context& context, double& diagnosticsMs) const;
    void releaseRenderReferences(Context& context) const;
};

} // namespace earth_engine
