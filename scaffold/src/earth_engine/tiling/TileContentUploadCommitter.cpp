#include "TileContentUploadCommitter.h"

#include "TileContentUploadPolicy.h"

#include <utility>

namespace earth_engine {

void TileContentUploadCommitter::prepareRenderContent(
    TilesetTile& tile,
    TileLoadedContent&& content) {
    TileContentUploadPolicy::prepareGltfRenderContent(
        tile,
        std::move(content));
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
