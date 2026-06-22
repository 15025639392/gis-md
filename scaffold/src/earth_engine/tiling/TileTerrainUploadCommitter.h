#pragma once

#include "TileLoadTypes.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class RenderDevice;
class TilesetContentProvider;
struct DecodedHeightmap;
struct SurfaceTileMesh;
struct TileKey;
struct TilesetTile;

struct TileTerrainUploadCommitAction {
    bool resourcesDirty = false;
};

struct TileTerrainUploadCommitter {
    static void applyAvailabilityUpdates(
        TilesetContentProvider* contentProvider,
        TerrainProvider* terrainProvider,
        const TileLoadedContent& content);

    static void cacheTerrainPayload(
        const std::string& cacheKey,
        TileLoadedContent& content,
        std::unordered_map<
            std::string,
            std::unique_ptr<DecodedHeightmap>>& terrainCache) {
        if (content.terrainPayloadKind == TerrainTilePayloadKind::Heightmap &&
            content.heightmap) {
            terrainCache[cacheKey] = std::move(content.heightmap);
        }
    }

    static void prepareTerrainRenderContent(
        TilesetTile& tile,
        TileLoadedContent&& content,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        RenderDevice* device);
    static void prepareTerrainRenderContent(TilesetTile& tile);
    static TileTerrainUploadCommitAction finishTerrainResourcePreparation(
        TilesetTile& tile,
        bool resourcesReady);
};

} // namespace earth_engine
