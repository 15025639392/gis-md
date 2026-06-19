#pragma once

#include "../tiling/TileOcclusionCallback.h"

#include <memory>
#include <vector>

namespace earth_engine {

struct FrameState;
class Tileset;

struct SceneTilesetUpdateResult {
    double terrainUpdateMs = 0.0;
    double contentTilesetUpdateMs = 0.0;
};

class SceneTilesetCoordinator {
public:
    ~SceneTilesetCoordinator();

    void setPrimary(std::unique_ptr<Tileset> tileset);
    void addContent(std::unique_ptr<Tileset> tileset);

    Tileset* primary() const { return primary_.get(); }
    const std::vector<std::unique_ptr<Tileset>>& contentTilesets() const {
        return contentTilesets_;
    }
    size_t contentTilesetCount() const { return contentTilesets_.size(); }
    bool hasPrimaryTerrain() const { return primary_ != nullptr; }

    void setOcclusionCallback(TileOcclusionCallback callback);
    void clearOcclusionCallback();
    SceneTilesetUpdateResult update(FrameState& frameState);

private:
    void applyOcclusionCallback(Tileset& tileset) const;

    std::unique_ptr<Tileset> primary_;
    std::vector<std::unique_ptr<Tileset>> contentTilesets_;
    TileOcclusionCallback occlusionCallback_;
};

} // namespace earth_engine
