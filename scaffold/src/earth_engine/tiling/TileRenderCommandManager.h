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
                    double currentFrameTimeSeconds,
                    const TerrainEdgeLutTableMap* edgeLutTables = nullptr);

    void buildTileDrawCommand(
        Renderer& renderer,
        TilesetTile& tile,
        RenderCommandList& commands,
        float transitionOpacity,
        bool allowSynchronousMeshPrep = true,
        const std::optional<std::array<float, 4>>& surfaceClipUv =
            std::nullopt,
        const TilesetTile* surfaceClipDescendant = nullptr);

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
    // ①-1(A′):本帧边高度差表(指向 TilePlan::edgeLutTables,plan 生存期
    // 覆盖整个 draw 阶段)。每帧 beginFrame 重新指认。
    const TerrainEdgeLutTableMap* edgeLutTables_ = nullptr;
    TileRenderCommandPerformanceTimings frameTimings_;
};

} // namespace earth_engine
