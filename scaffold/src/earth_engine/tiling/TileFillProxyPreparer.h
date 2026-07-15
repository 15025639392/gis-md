#pragma once

namespace earth_engine {

class RenderDevice;
class IPrepareRendererResources;
struct TilesetTile;

struct TileFillProxyPrepareResult {
    bool madeReady = false;
    bool resourcesChanged = false;

    explicit operator bool() const { return madeReady; }
};

// Synthesizes and GPU-uploads a smooth ellipsoid fill proxy for a tile whose
// real terrain is still loading, so imagery can drape on the globe immediately
// (proxy now, real terrain when ready). The proxy is a small static grid in the
// tile's separate fill slot; real gltf content is untouched.
class TileFillProxyPreparer {
public:
    /// Ensure `tile` has a ready fill proxy if it needs one this frame.
    /// No-op (returns false) when the tile already has real drawable content,
    /// already has a fill, is not terrain render content territory, or has no
    /// finite bounds. Returns true if a fill proxy was newly made ready.
    /// The proxy stays on the smooth ellipsoid until real terrain replaces it.
    /// Main thread only (creates GPU buffers).
    static TileFillProxyPrepareResult ensureFillProxy(
        TilesetTile& tile,
        RenderDevice* device,
        int gridSize,
        IPrepareRendererResources* pPrepRenderer = nullptr);
};

} // namespace earth_engine
