#pragma once

#include "../renderer/Renderer.h"
#include "TileRenderFrameCoordinator.h"

#include <array>
#include <optional>
#include <string>

namespace earth_engine {

class TileCacheOwnershipManager;
class TileContentAccess;
class TileRenderCommandManager;

struct TileRenderFrameContext {
    TileRenderFrameCoordinatorInput input;
    TileContentAccess& contentAccess;
    TileRenderCommandManager& renderCommands;
    TileCacheOwnershipManager& cacheOwnership;

    TilesetTile* ensureTile(const TileKey& key) const;

    void markIneligibleForUnloading(const std::string& cacheKey) const;

    void buildTileDrawCommand(
        Renderer& renderer,
        TilesetTile& tile,
        RenderCommandList& commands,
        float transitionOpacity,
        bool allowSynchronousMeshPrep,
        const std::optional<std::array<float, 4>>& surfaceClipUv) const;

    void markEligibleForUnloading(const std::string& cacheKey) const;

    void updateTotalBytesUsed() const;

    void unloadCachedBytes(Renderer& renderer) const;
};

} // namespace earth_engine
