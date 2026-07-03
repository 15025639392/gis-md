#pragma once

#include "TileLoadLifecycle.h"
#include "TileLoadRequestDispatcher.h"
#include "TileLoadRequestPlanner.h"
#include "TileLoadPriorityPolicy.h"
#include "TileLoadTypes.h"
#include "TileGltfTerrainUpsampledChildMaterializer.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../content/GltfContentProvider.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace earth_engine {

class TerrainProvider;
class TilesetContentProvider;
struct TilesetTile;

struct TileLoadSchedulerInput {
    TileLoadLifecycle& lifecycle;
    FrameResourceBudget& budget;
    TilesetContentProvider* contentProvider = nullptr;
};

class TileLoadScheduler {
public:
    template <typename CacheKeyForTileFn,
              typename MakeSnapshotFn,
              typename IsEmptyTileFn,
              typename PrepareUpsampleSourceTileFn,
              typename MarkTileContentLoadingFn>
    static TileLoadRequestOutcome requestMissingTiles(
        const std::vector<TileLoadRequest>& loadRequests,
        TileLoadSchedulerInput input,
        CacheKeyForTileFn&& cacheKeyForTile,
        MakeSnapshotFn&& makeSnapshot,
        IsEmptyTileFn&& isEmptyTile,
        PrepareUpsampleSourceTileFn&& prepareUpsampleSourceTile,
        MarkTileContentLoadingFn&& markTileContentLoading) {
        TileLoadRequestOutcome outcome;
        std::vector<TileLoadRequest> sorted = loadRequests;
        TileLoadPriorityPolicy::sortByPriority(sorted);

        for (const TileLoadRequest& request : sorted) {
            {
                std::lock_guard<std::mutex> lock(input.lifecycle.mutex());
                if (input.lifecycle.requestState().destroying()) {
                    break;
                }
            }

            const TileKey requestKey = request.key;
            const std::string cacheKey = cacheKeyForTile(requestKey);
            if (cacheKey.empty()) {
                ++outcome.skippedEmptyCacheKey;
                continue;
            }
            if (input.lifecycle.containsWorkForCacheKey(cacheKey)) {
                ++outcome.skippedAlreadyPending;
                continue;
            }
            if (isEmptyTile(cacheKey)) {
                ++outcome.skippedEmptyTile;
                continue;
            }

            TilesetTile* tileState = nullptr;
            const TileLoadRequestSnapshot snapshot =
                makeSnapshot(requestKey, cacheKey, tileState);
            const TileLoadRequestKind requestKind =
                TileLoadRequestPlanner::classify(snapshot);

            if (requestKind == TileLoadRequestKind::Skip) {
                ++outcome.skippedClassified;
                continue;
            }

            if (requestKind == TileLoadRequestKind::TerrainContentUpsample) {
                if (!tileState ||
                    !prepareUpsampleSourceTile(
                        *tileState,
                        request.priority)) {
                    ++outcome.skippedUpsampleSourceNotReady;
                    continue;
                }

                const bool needsRasterDetailUpsample =
                    tileState->content.isRasterDetailUpsample();
                const bool hasTerrainContentSource =
                    TileGltfTerrainUpsampledChildMaterializer::
                        canCreateLoadResult(*tileState);
                if (needsRasterDetailUpsample &&
                    !hasTerrainContentSource) {
                    ++outcome.skippedUpsampleNoContentSource;
                    continue;
                }
                if (!hasTerrainContentSource) {
                    ++outcome.skippedUpsampleNoContentSource;
                    continue;
                }
                std::optional<TileLoadResult> gltfUpsample =
                    TileGltfTerrainUpsampledChildMaterializer::
                        createLoadResult(*tileState);
                if (!gltfUpsample) {
                    gltfUpsample =
                        TileLoadResult::createTerminal(TileLoadStatus::Failed);
                }
                const TileLoadDispatchResult dispatchResult =
                    TileLoadRequestDispatcher::queueUpsampledLoad(
                        input.lifecycle.mutex(),
                        input.lifecycle.requestState(),
                        input.lifecycle.pendingLoads(),
                        requestKey,
                        cacheKey,
                        request.group,
                        request.priority,
                        TileLoadDomain::TerrainContent,
                        std::move(*gltfUpsample));
                if (shouldStopAfterDispatch(dispatchResult)) {
                    ++outcome.stoppedAtDispatch;
                    break;
                }
                if (dispatchResult == TileLoadDispatchResult::Skipped) {
                    ++outcome.skippedDispatch;
                    continue;
                }
                markTileContentLoading(requestKey);
                ++outcome.issued;
                continue;
            }

            if (requestKind == TileLoadRequestKind::Content) {
                if (!input.contentProvider) {
                    ++outcome.skippedNoContentProvider;
                    continue;
                }
                const int estimatedFanout =
                    input.contentProvider->estimatedRequestFanout(requestKey);
                const FrameResourceLane requestLane =
                    input.contentProvider->providesTerrainQuadtree()
                        ? FrameResourceLane::TerrainRequest
                        : FrameResourceLane::ContentRequest;
                {
                    std::lock_guard<std::mutex> lock(input.lifecycle.mutex());
                    if (!input.budget.hasNetworkInflightCapacity(
                            requestLane,
                            static_cast<uint32_t>(
                                input.lifecycle
                                    .requestState()
                                    .totalRequestCount()),
                            estimatedFanout)) {
                        outcome.blockedByInflight = true;
                        break;
                    }
                }
                const TileLoadDispatchResult dispatchResult =
                    TileLoadRequestDispatcher::requestContent(
                        input.lifecycle.mutex(),
                        input.lifecycle.condition(),
                        input.lifecycle.requestState(),
                        input.lifecycle.pendingLoads(),
                        input.budget,
                        *input.contentProvider,
                        requestKey,
                        cacheKey,
                        request.group,
                        request.priority,
                        [&markTileContentLoading, &requestKey, &outcome]() {
                            markTileContentLoading(requestKey);
                            ++outcome.issued;
                        },
                        requestOptionsForTile(*input.contentProvider, tileState));
                if (shouldStopAfterDispatch(dispatchResult)) {
                    ++outcome.stoppedAtDispatch;
                    break;
                }
                if (dispatchResult == TileLoadDispatchResult::Skipped) {
                    ++outcome.skippedDispatch;
                }
                continue;
            }
        }

        return outcome;
    }

private:
    static TileContentRequestOptions requestOptionsForTile(
        const TilesetContentProvider& provider,
        const TilesetTile* tile) {
        TileContentRequestOptions options;
        if (!provider.providesTerrainQuadtree() || !tile) {
            return options;
        }
        for (const TilesetTile* child : tile->children) {
            if (child && child->content.isTerrainAvailabilityUpsample()) {
                options.generateTerrainRasterOverlayDetails = true;
                break;
            }
        }
        return options;
    }

    static bool shouldStopAfterDispatch(TileLoadDispatchResult result) {
        return result == TileLoadDispatchResult::Destroying ||
               result == TileLoadDispatchResult::Blocked;
    }
};

} // namespace earth_engine
