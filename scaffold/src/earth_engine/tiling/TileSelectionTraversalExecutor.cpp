#include "TileSelectionTraversalExecutor.h"
#include "Tileset.h"
#include "TileContentAccess.h"
#include "TileSelectionChildTraversal.h"
#include "TileSelectionPlanAppender.h"
#include "TileSelectionTraversalDetailsBuilder.h"
#include "TileSelectionFrameBuilder.h"
#include "TileSelectionHistory.h"
#include "TileSelectionPostTraversalCommitter.h"
#include "TileSelectionPostTraversalPolicy.h"
#include "TileSelectionPreTraversalPolicy.h"
#include "TileSelectionRasterOverlayPreparer.h"
#include "TileSelectionRefineFlowPolicy.h"
#include "TileSelectionRootPolicy.h"
#include "TileSelectionTraversalCounterPolicy.h"
#include "TileSelectionVisibilitySampler.h"
#include "TileSelectionVisitPreparation.h"
#include "TilesetTile.h"
#include "../debug/PerfTimer.h"

#include <cmath>
#include <optional>

namespace earth_engine {
namespace {

void kickVisitedDescendants(TilesetTile& tile) {
    for (TilesetTile* child : tile.children) {
        if (!child) {
            continue;
        }
        kickSelectionState(child->selectionFrameState.selectionState);
        kickVisitedDescendants(*child);
    }
}

} // namespace

TileTraversalDetails TileSelectionTraversalExecutor::visitTileIfNeeded(
    TileSelectionTraversalContext& context,
    TilesetTile& tile,
    const SelectorFrame& selectorFrame,
    uint32_t depth,
    bool ancestorMeetsSse) {
    // Register + lazily reset this tile before any selection-state use, so the
    // per-frame reset touches only visited tiles (incremental active-set).
    context.onVisitTile(tile);
    // ③ 增量捕获:在子树 visit 加入任何 plan/loadQueue 条目之前快照(no-op 当
    // incremental==nullptr)。退出时记录 [snapshot, 末尾) 净贡献。
    const TileIncrementalFrontier::Snapshot incrementalSnapshot =
        context.beginIncrementalSubtree();
    TileSelectionFrameState& selection = tile.selectionFrameState;
    selection.ancestorMeetsSse = ancestorMeetsSse;
    const TileSelectionVisibilityContext visibilityContext{
        context.options.renderTilesUnderCamera,
        context.cameraLongitude,
        context.cameraLatitude};

    const TileSelectionVisitPreparationResult preparation =
        TileSelectionVisitPreparation::prepare(
            tile,
            selectorFrame.views,
            selectorFrame.fogDensities,
            visibilityContext,
            TileSelectionVisitPreparationOptions{
                context.options.enableFrustumCulling,
                context.options.enableFogCulling,
                context.options.preloadSiblings,
                context.options.forbidHoles,
                context.options.enforceCulledScreenSpaceError,
                context.options.maximumScreenSpaceError,
                context.options.culledScreenSpaceError},
            context.scratchDistances);
    selection.inFrustum = preparation.visibilitySample.inFrustum;
    selection.cameraInside = preparation.visibilitySample.cameraInside;
    selection.priority = preparation.inputSummary.priority;
    if (context.performanceTimings) {
        context.performanceTimings->visitVisibilityMs +=
            preparation.visibilityMs;
        context.performanceTimings->visitInputMetricsMs +=
            preparation.inputMetricsMs;
        context.performanceTimings->visitPolicyMs +=
            preparation.policyMs;
    }

    // 子树内到主阈值的 margin(离翻转最近);子树聚合见 TileIncrementalFrontier。
    const double tileMargin = std::abs(
        preparation.inputSummary.screenSpaceError -
        context.options.maximumScreenSpaceError);

    const TileSelectionVisitOutcomePlan visitOutcome =
        TileSelectionVisitPreparation::outcomePlan(preparation);
    const TileSelectionTraversalCounterPlan outcomeCounters =
        TileSelectionTraversalCounterPolicy::planOutcome(visitOutcome);
    context.counters.culled += outcomeCounters.frustumCulled;
    context.counters.fogCulled += outcomeCounters.fogCulled;
    context.counters.culledVisited += outcomeCounters.culledVisited;
    if (visitOutcome.shouldExit) {
        if (visitOutcome.markTileCulled) {
            selection.selectionState = TileSelectionState::Culled;
        }
        if (visitOutcome.resetScreenSpaceError) {
            selection.screenSpaceError = 0.0;
        }
        if (visitOutcome.queueLoad) {
            TileSelectionPlanAppender::queueTileLoad(
                context.loadQueue,
                tile.key,
                visitOutcome.loadGroup,
                preparation.inputSummary.priority);
        }
        return context.recordIncrementalSubtree(
            tile,
            incrementalSnapshot,
            visitOutcome.returnCulledTraversalDetails
                ? TileSelectionTraversalDetailsBuilder::forCulledTile(
                      tile,
                      context.rasterOverlays,
                      context.options.forbidHoles)
                : TileTraversalDetails{},
            tileMargin);
    }

    const TileSelectionTraversalCounterPlan visitAcceptedCounters =
        TileSelectionTraversalCounterPolicy::planVisitAccepted();
    context.counters.visited += visitAcceptedCounters.visited;

    return context.recordIncrementalSubtree(
        tile,
        incrementalSnapshot,
        visitTile(context,
                  tile,
                  selectorFrame,
                  depth,
                  preparation.meetsScreenSpaceError,
                  ancestorMeetsSse,
                  preparation.inputSummary.priority,
                  preparation.inputSummary.screenSpaceError),
        tileMargin);
}

TileTraversalDetails TileSelectionTraversalExecutor::visitTile(
    TileSelectionTraversalContext& context,
    TilesetTile& tile,
    const SelectorFrame& selectorFrame,
    uint32_t depth,
    bool meetsSse,
    bool ancestorMeetsSse,
    double tilePriority,
    double tileSse) {
    (void)depth;
    const double refinePreStartMs = perf::nowMs();
    const double refineOverlayStartMs = perf::nowMs();
    TileSelectionRasterOverlayPreparer::prepare(
        tile,
        context.rasterOverlays,
        context.device,
        context.options.maximumScreenSpaceError,
        context.frameResourceBudget,
        context.tilePlan.frameId,
        context.pPrepRenderer);
    const TileSelectionFrameState& selection = tile.selectionFrameState;
    const bool renderable =
        !TileSelectionRootPolicy::isVirtualTerrainRoot(tile.key) &&
        TileSelectionRasterOverlayPreparer::isRenderable(
            tile,
            context.rasterOverlays);
    tile.updateTraversalRenderability(renderable);
    if (context.performanceTimings) {
        context.performanceTimings->refineOverlayMs +=
            perf::nowMs() - refineOverlayStartMs;
    }

    const double refineDecisionStartMs = perf::nowMs();
    const bool tileCanRefine = context.contentAccess.canRefine(tile);
    TileSelectionRefineFlowResult refineFlow;
    refineFlow.ancestorMeetsSse = ancestorMeetsSse;
    if (tileCanRefine) {
        const TileSelectionRefineFlowOptions refineFlowOptions{
            context.options.enableOcclusionCulling,
            context.options.delayRefinementForOcclusion};
        TileSelectionRefineFlowInput refineFlowInput{
            tile.unconditionallyRefine,
            meetsSse,
            ancestorMeetsSse,
            renderable,
            selection.previousSelectionState,
            TileSelectionHistory::childWasRefinedLastFrame(tile),
            std::nullopt};
        refineFlow = TileSelectionRefineFlowPolicy::evaluate(
            refineFlowInput,
            refineFlowOptions);

        if (refineFlow.shouldCheckOcclusion) {
            refineFlowInput.occlusion = context.checkOcclusion(tile);
            refineFlow = TileSelectionRefineFlowPolicy::evaluate(
                refineFlowInput,
                refineFlowOptions);
            const TileSelectionTraversalCounterPlan occlusionCounters =
                TileSelectionTraversalCounterPolicy::planRefineFlow(
                    refineFlow);
            context.counters.occluded +=
                occlusionCounters.occluded;
            context.counters.waitingForOcclusionResults +=
                occlusionCounters.waitingForOcclusion;
        }
    }

    meetsSse = refineFlow.meetsScreenSpaceError;
    const TileSelectionPreTraversalPlan preTraversal =
        TileSelectionPreTraversalPolicy::plan(
            TileSelectionPreTraversalInput{
                tileCanRefine,
                tile.refine,
                refineFlow});
    ancestorMeetsSse = preTraversal.ancestorMeetsSseAfterPreTraversal;
    bool queuedForLoad = preTraversal.queuedForLoadAfterPreTraversal;
    if (context.performanceTimings) {
        context.performanceTimings->refineDecisionMs +=
            perf::nowMs() - refineDecisionStartMs;
    }

    const double refinePreCommitStartMs = perf::nowMs();
    if (preTraversal.queueUrgentLoad) {
        TileSelectionPlanAppender::queueTileLoad(
            context.loadQueue,
            tile.key,
            TileLoadPriorityGroup::Urgent,
            tilePriority);
    }

    if (preTraversal.finishAsSingleTile) {
        TileSelectionPlanAppender::addTileToCurrentPlan(
            context.tilePlan,
            context.loadQueue,
            context.options.enableLodTransitionPeriod,
            tile,
            tileSse,
            preTraversal.singleTileShouldQueueLoad,
            tilePriority);
        if (context.performanceTimings) {
            context.performanceTimings->refineCommitMs +=
                perf::nowMs() - refinePreCommitStartMs;
            context.performanceTimings->refineMs +=
                perf::nowMs() - refinePreStartMs;
        }
        return TileSelectionTraversalDetailsBuilder::forSingleTile(
            tile,
            context.rasterOverlays);
    }
    if (context.performanceTimings) {
        context.performanceTimings->refineCommitMs +=
            perf::nowMs() - refinePreCommitStartMs;
    }

    const double refineMaterializeStartMs = perf::nowMs();
    const TileChildFrameMaterializeResult childMaterialize =
        context.contentAccess.ensureTileChildren(tile);
    if (context.performanceTimings) {
        context.performanceTimings->refineMaterializeMs +=
            perf::nowMs() - refineMaterializeStartMs;
        ++context.performanceTimings->refineMaterializeCalls;
        context.performanceTimings->refineMaterializeChanged +=
            childMaterialize.changed ? 1 : 0;
        context.performanceTimings->refineMaterializeRetry +=
            childMaterialize.retryLater ? 1 : 0;
        context.performanceTimings->refineMaterializeFastPath +=
            childMaterialize.fastPath ? 1 : 0;
    }

    const double refineMaterializeCommitStartMs = perf::nowMs();
    if (childMaterialize.retryLater) {
        TileSelectionPlanAppender::queueTileLoad(
            context.loadQueue,
            tile.key,
            TileLoadPriorityGroup::Urgent,
            tilePriority);
        queuedForLoad = true;
    }

    if (preTraversal.addAdditiveParentToPlan) {
        TileSelectionPlanAppender::addTileToCurrentPlan(
            context.tilePlan,
            context.loadQueue,
            context.options.enableLodTransitionPeriod,
            tile,
            tileSse,
            preTraversal.additiveParentShouldQueueLoad,
            tilePriority);
    }
    if (context.performanceTimings) {
        context.performanceTimings->refineCommitMs +=
            perf::nowMs() - refineMaterializeCommitStartMs;
    }

    const size_t firstRenderedDescendant =
        context.tilePlan.visibleTiles.size();
    const size_t loadQueueBeforeChildren = context.loadQueue.size();

    if (context.performanceTimings) {
        context.performanceTimings->refineMs +=
            perf::nowMs() - refinePreStartMs;
    }
    const TileTraversalDetails traversalDetails =
        TileSelectionChildTraversal::visitChildren(
            tile.children,
            [&context, &selectorFrame, depth, ancestorMeetsSse](
                TilesetTile& child) {
                return visitTileIfNeeded(
                    context,
                    child,
                    selectorFrame,
                    depth + 1,
                    ancestorMeetsSse);
            });

    const double refinePostStartMs = perf::nowMs();
    const TileSelectionPostTraversalResult postTraversal =
        TileSelectionPostTraversalPolicy::evaluate(
            TileSelectionPostTraversalInput{
                traversalDetails,
                renderable,
                tile.unconditionallyRefine,
                selection.previousSelectionState,
                tile.content.contentKind == TileContentKind::Render &&
                    TileSelectionRasterOverlayPreparer::isRenderable(
                        tile,
                        context.rasterOverlays),
                selection.lodTransitionFadePercentage,
                TileSelectionHistory::wasRenderedLastFrame(tile),
                tile.content.contentKind == TileContentKind::External,
                tile.refine,
                queuedForLoad},
            TileSelectionPostTraversalOptions{
                context.options.loadingDescendantLimit,
                context.options.enableLodTransitionPeriod,
                context.options.kickDescendantsWhileFadingIn,
                context.options.preloadAncestors});

    const TileSelectionPostTraversalCommitPlan postCommit =
        TileSelectionPostTraversalPolicy::commitPlan(
            postTraversal,
            queuedForLoad);

    const TileSelectionPostTraversalCommitResult commitResult =
        TileSelectionPostTraversalCommitter::commit(
            tile,
            context.tilePlan,
            context.loadQueue,
            context.counters,
            postCommit,
            TileSelectionPostTraversalCommitContext{
                firstRenderedDescendant,
                loadQueueBeforeChildren,
                tileSse,
                tilePriority,
                renderable,
                traversalDetails.notYetRenderableCount},
            [](TilesetTile& kickedTile) {
                kickVisitedDescendants(kickedTile);
            },
            [&context](const TileKey& key,
                       TileLoadPriorityGroup group,
                       double priority) {
                TileSelectionPlanAppender::queueTileLoad(
                    context.loadQueue, key, group, priority);
            },
            [&context](TilesetTile& selectedTile,
                       double screenSpaceError,
                       bool queueForLoad,
                       double priority) {
                TileSelectionPlanAppender::addTileToCurrentPlan(
                    context.tilePlan,
                    context.loadQueue,
                    context.options.enableLodTransitionPeriod,
                    selectedTile,
                    screenSpaceError,
                    queueForLoad,
                    priority);
            });

    if (context.performanceTimings) {
        context.performanceTimings->refineCommitMs +=
            perf::nowMs() - refinePostStartMs;
        context.performanceTimings->refineMs +=
            perf::nowMs() - refinePostStartMs;
    }
    if (commitResult.returnedSingleTileDetails) {
        return commitResult.details;
    }
    return traversalDetails;
}

} // namespace earth_engine
