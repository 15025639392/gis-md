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
#include "TileRenderPlanFinalizer.h"
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
                context.options.culledScreenSpaceError,
                context.performanceTimings &&
                    context.performanceTimings->collectDetailed},
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
        return visitOutcome.returnCulledTraversalDetails
            ? TileSelectionTraversalDetailsBuilder::forCulledTile(
                  tile,
                  context.rasterOverlays,
                  context.options.forbidHoles)
            : TileTraversalDetails{};
    }

    const TileSelectionTraversalCounterPlan visitAcceptedCounters =
        TileSelectionTraversalCounterPolicy::planVisitAccepted();
    context.counters.visited += visitAcceptedCounters.visited;

    return visitTile(context,
                     tile,
                     selectorFrame,
                     depth,
                     preparation.meetsScreenSpaceError,
                     ancestorMeetsSse,
                     preparation.inputSummary.priority,
                     preparation.inputSummary.screenSpaceError);
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
    const bool collectDetailedTimings =
        context.performanceTimings &&
        context.performanceTimings->collectDetailed;
    const double refinePreStartMs =
        collectDetailedTimings ? perf::nowMs() : 0.0;
    const double refineOverlayStartMs =
        collectDetailedTimings ? perf::nowMs() : 0.0;
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
    // 呈现级可画:向下回落守卫(continueDeeper:上帧 Refined 且不可画 →
    // 后代留任)的输入必须与 finalizer 的建条判据同源 —— 遍历级 renderable
    // 只看 mapping 就绪位,与绑定级判据在暂态分叉时守卫不触发,瓦片被选中却
    // 建不出条目 = 外拉漏底(cesium-js QuadtreePrimitive 的对应分支:"keep
    // rendering level 15…rendering level zero would be pretty jarring")。
    // Empty/External 内容本就不产几何条目,维持遍历级判定;Failed 无内容者
    // 走严判 → 后代留任优于空洞。
    const bool presentable =
        renderable &&
        (tile.content.contentKind == TileContentKind::Empty ||
         tile.content.contentKind == TileContentKind::External ||
         TileRenderPlanFinalizer::canBuildRenderEntryDirectly(
             tile,
             context.rasterOverlays,
             TileRenderPlanFinalizer::DirectRenderFallbackPolicy::
                 AllowTransientSurfaceAsLastResort));
    if (collectDetailedTimings) {
        context.performanceTimings->refineOverlayMs +=
            perf::nowMs() - refineOverlayStartMs;
    }

    const double refineDecisionStartMs =
        collectDetailedTimings ? perf::nowMs() : 0.0;
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
            // 守卫用呈现级判定(见上):与 finalizer 建条同源,分叉即漏底。
            presentable,
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
    if (collectDetailedTimings) {
        context.performanceTimings->refineDecisionMs +=
            perf::nowMs() - refineDecisionStartMs;
    }

    const double refinePreCommitStartMs =
        collectDetailedTimings ? perf::nowMs() : 0.0;
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
            tile,
            tileSse,
            preTraversal.singleTileShouldQueueLoad,
            tilePriority);
        if (collectDetailedTimings) {
            context.performanceTimings->refineCommitMs +=
                perf::nowMs() - refinePreCommitStartMs;
            context.performanceTimings->refineMs +=
                perf::nowMs() - refinePreStartMs;
        }
        return TileSelectionTraversalDetailsBuilder::forSingleTile(
            tile,
            context.rasterOverlays);
    }
    if (collectDetailedTimings) {
        context.performanceTimings->refineCommitMs +=
            perf::nowMs() - refinePreCommitStartMs;
    }

    const double refineMaterializeStartMs =
        collectDetailedTimings ? perf::nowMs() : 0.0;
    const TileChildFrameMaterializeResult childMaterialize =
        context.contentAccess.ensureTileChildren(tile);
    if (collectDetailedTimings) {
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

    const double refineMaterializeCommitStartMs =
        collectDetailedTimings ? perf::nowMs() : 0.0;
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
            tile,
            tileSse,
            preTraversal.additiveParentShouldQueueLoad,
            tilePriority);
    }
    if (collectDetailedTimings) {
        context.performanceTimings->refineCommitMs +=
            perf::nowMs() - refineMaterializeCommitStartMs;
    }

    const size_t firstRenderedDescendant =
        context.tilePlan.visibleTiles.size();
    const size_t loadQueueBeforeChildren = context.loadQueue.size();

    if (collectDetailedTimings) {
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

    const double refinePostStartMs =
        collectDetailedTimings ? perf::nowMs() : 0.0;
    const TileSelectionPostTraversalResult postTraversal =
        TileSelectionPostTraversalPolicy::evaluate(
            TileSelectionPostTraversalInput{
                traversalDetails,
                renderable,
                tile.unconditionallyRefine,
                TileSelectionHistory::wasRenderedLastFrame(tile),
                tile.content.contentKind == TileContentKind::External,
                tile.refine,
                queuedForLoad},
            TileSelectionPostTraversalOptions{
                context.options.loadingDescendantLimit,
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
                    selectedTile,
                    screenSpaceError,
                    queueForLoad,
                    priority);
            });

    if (collectDetailedTimings) {
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
