#include "TileRenderablePolicy.h"

namespace earth_engine {

bool TileRenderablePolicy::isCompleteRenderable(
    const TileRenderableSnapshot& snapshot) {
    if (snapshot.loadState == TileLoadState::Failed) {
        return true;
    }

    if (snapshot.loadState != TileLoadState::Done) {
        return false;
    }

    if (snapshot.unconditionallyRefine && snapshot.hasChildren) {
        return false;
    }

    switch (snapshot.contentKind) {
        case TileContentKind::Empty:
        case TileContentKind::External:
            return true;
        case TileContentKind::Render:
            return snapshot.requiredRasterOverlaysReady &&
                   snapshot.meshReady;
        case TileContentKind::Unknown:
            return false;
    }

    return false;
}

bool TileRenderablePolicy::hasSurfaceDrawable(
    TileContentKind contentKind,
    TileLoadState loadState,
    bool meshReady,
    bool hasGpuVertexBuffer) {
    if (contentKind == TileContentKind::Render) {
        return meshReady && hasGpuVertexBuffer;
    }

    if (contentKind == TileContentKind::External ||
        contentKind == TileContentKind::Empty) {
        return loadState == TileLoadState::Done ||
               loadState == TileLoadState::Failed;
    }

    return false;
}

} // namespace earth_engine
