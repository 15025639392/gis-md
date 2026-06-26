#pragma once

namespace earth_engine {

class RenderDevice;
struct TilesetTile;

struct SurfaceMeshResourcePreparer {
    static void prepare(TilesetTile& tile, RenderDevice* device);
};

} // namespace earth_engine
