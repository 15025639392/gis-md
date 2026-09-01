#pragma once

#include "../renderer/Renderer.h"
#include "TileRenderFrameCoordinator.h"
#include "TileRenderReferenceReleaser.h"

#include <array>
#include <optional>
#include <string>

namespace earth_engine {

class TileCacheOwnershipManager;
class TileRenderCommandManager;
struct TileRenderCommandPerformanceTimings;

struct TileRenderFrameContext {
    TileRenderFrameCoordinatorInput input;
    TileRenderCommandManager& renderCommands;
    TileCacheOwnershipManager& cacheOwnership;
    std::vector<TileRenderReference>& renderReferences;

    void markIneligibleForUnloading(const std::string& cacheKey) const;
    void trackRenderReference(
        TilesetTile* tile,
        std::string cacheKey,
        bool countedReference) const;

    void buildTileDrawCommand(
        Renderer& renderer,
        TilesetTile& tile,
        RenderCommandList& commands,
        float transitionOpacity,
        bool allowSynchronousMeshPrep,
        const std::optional<std::array<float, 4>>& surfaceClipUv,
        const TilesetTile* terrainFillMaskOwner = nullptr,
        const TilesetTile* surfaceClipDescendant = nullptr) const;
    const TileRenderCommandPerformanceTimings&
    renderCommandTimings() const;

    void trimRasterCaches(bool cachePressure) const;
    bool shouldUnloadCachedBytes() const;
    void unloadCachedBytes(Renderer& renderer) const;
};

} // namespace earth_engine
