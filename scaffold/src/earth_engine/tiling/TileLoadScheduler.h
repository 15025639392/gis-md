#pragma once

#include "TileLoadLifecycle.h"
#include "TileLoadRequestDispatcher.h"
#include "TileLoadRequestPlanner.h"
#include "TileLoadPriorityPolicy.h"
#include "TileLoadTypes.h"
#include "TileGltfTerrainUpsampledChildMaterializer.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../content/GltfContentProvider.h"
#include "../providers/TerrainProvider.h"

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
    TerrainProvider* legacyTerrainProvider = nullptr;
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
                continue;
            }
            if (input.lifecycle.containsWorkForCacheKey(cacheKey)) {
                continue;
            }
            if (isEmptyTile(cacheKey)) {
                continue;
            }

            TilesetTile* tileState = nullptr;
            const TileLoadRequestSnapshot snapshot =
                makeSnapshot(requestKey, cacheKey, tileState);
            const TileLoadRequestKind requestKind =
                TileLoadRequestPlanner::classify(snapshot);

            if (requestKind == TileLoadRequestKind::Skip) {
                continue;
            }

            if (requestKind == TileLoadRequestKind::UpsampledTerrain) {
                if (!tileState ||
                    !prepareUpsampleSourceTile(
                        *tileState,
                        request.priority)) {
                    continue;
                }

                const bool hasGltfTerrainSource =
                    TileGltfTerrainUpsampledChildMaterializer::
                        findGltfTerrainSource(*tileState) != nullptr;
                if (snapshot.contentProviderOwnsTerrainQuadtree &&
                    !hasGltfTerrainSource) {
                    continue;
                }
                const TileLoadDomain upsampleDomain = hasGltfTerrainSource
                    ? TileLoadDomain::Content
                    : TileLoadDomain::Terrain;
                TileLoadResult upsampleResult =
                    TileLoadResult::createRenderable();
                if (hasGltfTerrainSource) {
                    std::optional<TileLoadResult> gltfUpsample =
                        TileGltfTerrainUpsampledChildMaterializer::
                            createLoadResult(*tileState);
                    if (!gltfUpsample) {
                        continue;
                    }
                    upsampleResult = std::move(*gltfUpsample);
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
                        upsampleDomain,
                        std::move(upsampleResult));
                if (shouldStopAfterDispatch(dispatchResult)) {
                    break;
                }
                if (dispatchResult == TileLoadDispatchResult::Skipped) {
                    continue;
                }
                markTileContentLoading(requestKey);
                ++outcome.issued;
                continue;
            }

            if (requestKind == TileLoadRequestKind::Content) {
                if (!input.contentProvider) {
                    continue;
                }
                const int estimatedFanout =
                    input.contentProvider->estimatedRequestFanout(requestKey);
                {
                    std::lock_guard<std::mutex> lock(input.lifecycle.mutex());
                    if (!input.budget.hasNetworkInflightCapacity(
                            FrameResourceLane::ContentRequest,
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
                        });
                if (shouldStopAfterDispatch(dispatchResult)) {
                    break;
                }
                continue;
            }

            if (requestKind != TileLoadRequestKind::Terrain ||
                !input.legacyTerrainProvider) {
                continue;
            }

            {
                const int estimatedFanout =
                    input.legacyTerrainProvider->estimatedRequestFanout(
                        requestKey);
                std::lock_guard<std::mutex> lock(input.lifecycle.mutex());
                if (!input.budget.hasNetworkInflightCapacity(
                        FrameResourceLane::TerrainRequest,
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
                TileLoadRequestDispatcher::requestTerrain(
                    input.lifecycle.mutex(),
                    input.lifecycle.condition(),
                    input.lifecycle.requestState(),
                    input.lifecycle.pendingLoads(),
                    input.budget,
                    *input.legacyTerrainProvider,
                    requestKey,
                    cacheKey,
                    request.group,
                    request.priority,
                    [&markTileContentLoading, &requestKey, &outcome]() {
                        markTileContentLoading(requestKey);
                        ++outcome.issued;
                    });
            if (shouldStopAfterDispatch(dispatchResult)) {
                break;
            }
        }

        return outcome;
    }

private:
    static bool shouldStopAfterDispatch(TileLoadDispatchResult result) {
        return result == TileLoadDispatchResult::Destroying ||
               result == TileLoadDispatchResult::Blocked;
    }
};

} // namespace earth_engine
