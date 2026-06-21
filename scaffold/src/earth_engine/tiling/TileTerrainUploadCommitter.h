#pragma once

#include "TileLoadTypes.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace earth_engine {

struct DecodedHeightmap;
struct SurfaceTileMesh;
struct TileKey;
struct TilesetTile;

struct TileTerrainUploadCommitAction {
    bool resourcesDirty = false;
};

struct TileTerrainUploadCommitter {
    static void applyAvailabilityUpdates(
        TerrainProvider* terrainProvider,
        const TileLoadedContent& content);

    template <typename IngestAvailabilityFn>
    static void ingestTerrainPayload(
        const TileKey& key,
        const std::string& cacheKey,
        TileLoadedContent& content,
        std::unordered_map<
            std::string,
            std::unique_ptr<DecodedHeightmap>>& terrainCache,
        IngestAvailabilityFn&& ingestAvailability) {
        if (content.heightmap) {
            ingestAvailability(
                key,
                content.heightmap.get(),
                content.surfaceMesh.get());
            terrainCache[cacheKey] = std::move(content.heightmap);
        } else if (content.surfaceMesh) {
            ingestAvailability(
                key,
                nullptr,
                content.surfaceMesh.get());
        }
    }

    static void prepareTerrainRenderContent(
        TilesetTile& tile,
        TileLoadedContent&& content);
    static void prepareTerrainRenderContent(TilesetTile& tile);
    static TileTerrainUploadCommitAction finishMeshResourcePreparation(
        TilesetTile& tile,
        bool resourcesReady);
};

} // namespace earth_engine
