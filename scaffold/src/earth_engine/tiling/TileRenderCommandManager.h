#pragma once

#include "TileRenderCommandPreparer.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class RenderDevice;
class Renderer;
class TileContentResourceInvalidator;
class TileMeshPreparationManager;
struct TileSelectionReuseState;
struct TilesetTile;

class TileRenderCommandManager {
public:
    TileRenderCommandManager(
        TileMeshPreparationManager& meshPreparation,
        TileContentResourceInvalidator& resourceInvalidator,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        RenderDevice* device);

    void beginFrame(uint64_t frameNumber,
                    uint64_t generation,
                    double currentFrameTimeSeconds);

    void buildTileDrawCommand(
        Renderer& renderer,
        TilesetTile& tile,
        RenderCommandList& commands,
        float transitionOpacity,
        bool allowSynchronousMeshPrep = true,
        const std::optional<std::array<float, 4>>& surfaceClipUv =
            std::nullopt);

    const TileRenderCommandPerformanceTimings& frameTimings() const {
        return frameTimings_;
    }

private:
    TileMeshPreparationManager& meshPreparation_;
    TileContentResourceInvalidator& resourceInvalidator_;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays_;
    RenderDevice* device_ = nullptr;
    uint64_t frameNumber_ = 0;
    uint64_t generation_ = 0;
    double currentFrameTimeSeconds_ = 0.0;
    TileRenderCommandPerformanceTimings frameTimings_;
};

} // namespace earth_engine
