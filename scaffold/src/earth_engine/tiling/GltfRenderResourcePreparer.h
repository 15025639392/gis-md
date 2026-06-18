#pragma once

namespace earth_engine {

class RenderDevice;
struct TilesetTile;

struct GltfRenderResourcePreparer {
    static void prepare(TilesetTile& tile,
                        RenderDevice* device,
                        double currentFrameTimeSeconds);
};

} // namespace earth_engine
