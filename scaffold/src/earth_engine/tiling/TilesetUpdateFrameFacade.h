#pragma once

namespace earth_engine {

struct FrameState;
class IPrepareRendererResources;
class SceneFrameResourceArbiter;
class Tileset;

class TilesetUpdateFrameFacade {
public:
    static void update(
        Tileset& tileset,
        const FrameState& frameState,
        IPrepareRendererResources* pPrepRenderer,
        SceneFrameResourceArbiter* resourceArbiter = nullptr);
};

} // namespace earth_engine
