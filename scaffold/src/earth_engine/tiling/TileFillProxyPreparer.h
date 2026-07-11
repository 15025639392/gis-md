#pragma once

#include <memory>
#include <string>
#include <unordered_map>

namespace earth_engine {

class RenderDevice;
struct TilesetTile;

// Synthesizes and GPU-uploads an ellipsoid fill proxy for a tile whose real
// terrain is still loading, so imagery can drape on the smooth globe
// immediately (cesium-js TerrainFillMesh model: proxy now, real terrain
// "rises" when it arrives). The proxy is a small static ellipsoid-surface grid
// (EllipsoidTerrainMeshBuilder) uploaded in the lightweight terrain vertex
// format into the tile's SEPARATE fill slot — real gltf* content is untouched.
class TileFillProxyPreparer {
public:
    /// Ensure `tile` has a ready fill proxy if it needs one this frame.
    /// No-op (returns false) when the tile already has real drawable content,
    /// already has a fill, is not terrain render content territory, or has no
    /// finite bounds. Returns true if a fill proxy was newly made ready.
    /// The proxy borrows loaded-terrain heights (sampled from `tiles`) along
    /// its grid so it meets loaded neighbours crack-free. Main thread only
    /// (creates GPU buffers).
    static bool ensureFillProxy(
        TilesetTile& tile,
        const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>&
            tiles,
        RenderDevice* device,
        int gridSize);
};

} // namespace earth_engine
