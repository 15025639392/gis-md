#pragma once

namespace earth_engine {

struct FrameState;
struct TilesetTile;
class Tileset;
class TileSelectionShadowRunner;
enum class TileOcclusionState;

class TilesetSelectionFrameFacade {
public:
    static void selectTiles(Tileset& tileset, const FrameState& frameState);

    // Land a finished async-worker selection, UNCONDITIONALLY each frame —
    // independent of the reuse gate. Must be called before the frame's reuse
    // decision (see TilesetUpdateFrameRuntime). No-op unless the true-async
    // (asyncSelectionNonBlocking) worker exists and has a ready result.
    static void consumeAsyncSelectionResult(Tileset& tileset);

private:
    // Synchronous (default) path: traverse the live tree directly.
    static void selectTilesSync(Tileset& tileset, const FrameState& frameState);

    // asyncSelection path (kill-switch). Dispatches to the synchronous-shadow or
    // true-async worker variant based on asyncSelectionNonBlocking.
    static void selectTilesAsyncShadow(Tileset& tileset,
                                       const FrameState& frameState);

    // asyncSelection && !nonBlocking: run the shadow selection synchronously on
    // the render thread, then reconcile. No latency → golden can frame-by-frame
    // verify the shadow selection algorithm equals the sync path / cesium.
    static void selectTilesSyncShadow(Tileset& tileset,
                                      const FrameState& frameState);

    // asyncSelection && nonBlocking: true async — consume any ready worker
    // result, then (if the worker is idle) snapshot + dispatch a new job;
    // otherwise keep drawing the last reconciled plan (fallback).
    static void selectTilesAsyncWorker(Tileset& tileset,
                                       const FrameState& frameState);

    // Copy the shadow selection result onto live (render/load keys, counters)
    // and write each shadow tile's selection state back to its live tile for
    // cross-frame history. Render-thread only.
    static void reconcileShadowToLive(Tileset& tileset,
                                      const TileSelectionShadowRunner& runner);

    // §8 等价性 oracle(见 docs/issues/selector-incremental-frontier-design-
    // 2026-07-06.md)。仅 sync 路径 + verifySelectionEquivalence 开启时调用:
    // 用「selectTiles 之前」build 的 pre-selection 影子跑一遍全量参考选择,
    // 与 live 结果比对,不一致时把 mismatch 写回 tileset(debug/test 可读)。
    // `preSelectionShadow` 必须在 live selectTiles 改状态之前 buildShadow。
    static void verifySelectionEquivalence(
        Tileset& tileset,
        const FrameState& frameState,
        TileSelectionShadowRunner& preSelectionShadow);

    // Occlusion thunk for the shadow traversal. Evaluates Tileset::checkOcclusion
    // on a shadow tile (bounds + camera based, so identical to the live tile).
    // Only used on the synchronous-shadow path — the async worker passes null
    // (a real thunk reads live mutable occlusion state that the render thread
    // updates, which would race).
    static TileOcclusionState shadowOcclusion(void* tilesetPtr,
                                              const TilesetTile& tile);
};

} // namespace earth_engine
