#include "TileContentUploadCommitter.h"

#include "TileContentUploadPolicy.h"

#include <utility>

namespace earth_engine {

void TileContentUploadCommitter::prepareRenderContent(
    TilesetTile& tile,
    TileContentLoadResult&& result) {
    TileContentUploadPolicy::prepareGltfRenderContent(
        tile,
        std::move(result));
}

TileContentUploadCommitAction
TileContentUploadCommitter::finishRenderResourcePreparation(
    TilesetTile& tile,
    bool resourcesReady) {
    if (!resourcesReady) {
        TileContentUploadPolicy::markGltfRenderResourcesFailed(tile);
    }
    return TileContentUploadCommitAction{true};
}

} // namespace earth_engine
