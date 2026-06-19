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
class TileCacheOwnershipManager;
class TileMeshPreparationManager;
class TileRasterUpsampledChildCoordinator;
struct TileSelectionReuseState;
struct TilesetTile;

class TileRenderCommandManager {
public:
    TileRenderCommandManager(
        TileMeshPreparationManager& meshPreparation,
        TileCacheOwnershipManager& cacheOwnership,
        TileRasterUpsampledChildCoordinator& rasterUpsampledChildren,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        RenderDevice* device,
        FrameResourceBudget& frameResourceBudget);

    void beginFrame(uint64_t frameNumber,
                    uint64_t generation,
                    double currentFrameTimeSeconds,
                    double maximumScreenSpaceError);

    void buildTileDrawCommand(
        Renderer& renderer,
        TilesetTile& tile,
        RenderCommandList& commands,
        float transitionOpacity,
        bool allowSynchronousMeshPrep = true,
        const std::optional<std::array<float, 4>>& surfaceClipUv =
            std::nullopt);

private:
    TileMeshPreparationManager& meshPreparation_;
    TileCacheOwnershipManager& cacheOwnership_;
    TileRasterUpsampledChildCoordinator& rasterUpsampledChildren_;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays_;
    RenderDevice* device_ = nullptr;
    FrameResourceBudget& frameResourceBudget_;
    uint64_t frameNumber_ = 0;
    uint64_t generation_ = 0;
    double currentFrameTimeSeconds_ = 0.0;
    double maximumScreenSpaceError_ = 16.0;
};

} // namespace earth_engine
