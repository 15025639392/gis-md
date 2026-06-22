#pragma once

namespace earth_engine {

struct FrameState;
class IPrepareRendererResources;
class Tileset;

class TilesetUpdateFrameFacade {
public:
    static void update(
        Tileset& tileset,
        const FrameState& frameState,
        IPrepareRendererResources* pPrepRenderer);
};

} // namespace earth_engine
