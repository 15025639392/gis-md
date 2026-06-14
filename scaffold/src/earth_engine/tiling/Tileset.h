#pragma once

#include "TilesetTile.h"
#include "RasterMappedToTilesetTile.h"
#include "TileQuadTree.h"
#include "TileScheme.h"
#include "TilePlan.h"
#include "../core/math/Vec3.h"
#include "../providers/TerrainProvider.h"
#include "../providers/ImageryProvider.h"
#include "../renderer/RenderCommand.h"

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <mutex>

namespace earth_engine {

class Renderer;
class RenderDevice;
struct FrameState;
class BasemapLayer;

/// cesium-native Tileset equivalent.
/// Manages a unified quadtree of terrain+imagery tiles.
/// Replaces BasemapLayer + TerrainLayer coordination.
class Tileset {
public:
    Tileset(std::unique_ptr<TerrainProvider> terrainProvider,
            std::unique_ptr<TileScheme> tileScheme,
            std::vector<BasemapLayer*> imageryLayers,
            RenderDevice* device);
    ~Tileset();

    void update(const FrameState& frameState);
    void buildRenderCommands(Renderer& renderer, RenderCommandList& commands);

    const TilePlan& tilePlan() const { return tilePlan_; }
    int cachedTerrainTiles() const { return static_cast<int>(terrainCache_.size()); }
    int pendingRequests() const { return static_cast<int>(pendingRequests_.size()); }

private:
    void requestMissingTiles(const std::vector<TileKey>& visibleKeys);
    void processPendingUploads();
    void ensureTileMesh(TilesetTile& tile);
    void buildTileDrawCommand(Renderer& renderer, TilesetTile& tile,
                              RenderCommandList& commands);
    std::string terrainCacheKey(const TileKey& key) const;

    std::unique_ptr<TerrainProvider> terrainProvider_;
    std::unique_ptr<TileScheme> tileScheme_;
    std::unique_ptr<TileQuadTree> quadTree_;
    std::vector<BasemapLayer*> imageryLayers_;
    RenderDevice* device_ = nullptr;

    TilePlan tilePlan_;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles_;
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>> terrainCache_;
    std::unordered_set<std::string> pendingRequests_;
    std::unordered_set<std::string> emptyTiles_;
    std::deque<std::pair<std::string, std::unique_ptr<DecodedHeightmap>>> pendingUploads_;
    std::mutex pendingMutex_;
    uint64_t generation_ = 0;
    Vec3 lastCameraPosition_ = Vec3::zero();
    bool cameraMoving_ = false;
};

} // namespace earth_engine
