#include "TileTerminalLoadCommitter.h"
#include "TileEmptyContentRegistry.h"
#include "TileLoadDomainPolicy.h"
#include "TileLoadResultMetadataApplicator.h"
#include <utility>
namespace earth_engine {
namespace {
TileTerminalLoadAction commitTerminalResultImpl(
    TileLoadDomain domain,
    TilesetTile& tile,
    const std::string& cacheKey,
    TileLoadResult result,
    TileEmptyContentRegistry& emptyContentRegistry,
    IPrepareRendererResources* pPrepRenderer) {
    TileTerminalLoadAction action =
        TileTerminalLoadPolicy::applyTerminalResult(
            domain,
            tile,
            result.status,
            pPrepRenderer);
    if (TileLoadDomainPolicy::shouldApplyTerminalMetadataForDomain(
            domain,
            result)) {
        TileLoadResultMetadataApplicator::apply(
            tile,
            std::move(result.content.metadata));
    }
    if (action.markEmptyCacheKey) {
        emptyContentRegistry.insert(cacheKey);
    } else {
        emptyContentRegistry.erase(cacheKey);
    }
    return action;
}
} // namespace
TileTerminalLoadAction
TileTerminalLoadCommitter::commitTerminalResult(
    TileLoadDomain domain,
    TilesetTile& tile,
    const std::string& cacheKey,
    TileLoadResult result,
    TileEmptyContentRegistry& emptyContentRegistry,
    IPrepareRendererResources* pPrepRenderer,
    TilesetContentProvider* contentProvider) {
    (void)contentProvider;
    TileTerminalLoadAction action = commitTerminalResultImpl(
        domain,
        tile,
        cacheKey,
        std::move(result),
        emptyContentRegistry,
        pPrepRenderer);
    return action;
}
} // namespace earth_engine
