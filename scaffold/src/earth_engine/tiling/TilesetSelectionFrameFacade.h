#pragma once

namespace earth_engine {

struct FrameState;
struct TilesetTile;
class Tileset;
enum class TileOcclusionState;

class TilesetSelectionFrameFacade {
public:
    static void selectTiles(Tileset& tileset, const FrameState& frameState);

private:
    // Synchronous (default) path: traverse the live tree directly.
    static void selectTilesSync(Tileset& tileset, const FrameState& frameState);

    // asyncSelection path (kill-switch): run the traversal on a shadow copy of
    // the live tree, then reconcile the result onto live. Currently still
    // synchronous (no worker thread yet) — this isolates the shadow-selection
    // correctness (golden equivalence) before threading is added.
    static void selectTilesAsyncShadow(Tileset& tileset,
                                       const FrameState& frameState);

    // Occlusion thunk for the shadow traversal. Evaluates Tileset::checkOcclusion
    // on a shadow tile (bounds + camera based, so identical to the live tile).
    static TileOcclusionState shadowOcclusion(void* tilesetPtr,
                                              const TilesetTile& tile);
};

} // namespace earth_engine
