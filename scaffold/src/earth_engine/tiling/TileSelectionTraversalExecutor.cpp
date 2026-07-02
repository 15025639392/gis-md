#include "TileSelectionTraversalExecutor.h"
#include "Tileset.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "TileSelectionChildTraversal.h"
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
    const Cartographic cameraCart =
        Ellipsoid::WGS84().cartesianToCartographic(
            context.lastCameraPosition);
    TileSelectionFrameState& selection = tile.selectionFrameState;
    selection.ancestorMeetsSse = ancestorMeetsSse;
    const TileSelectionVisibilityContext visibilityContext{
        context.options.renderTilesUnderCamera,
        cameraCart.longitude(),
        cameraCart.latitude()};

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
                context.options.culledScreenSpaceError});
    selection.inFrustum = preparation.visibilitySample.inFrustum;
    selection.cameraInside =
        TileSelectionVisibilitySampler::cameraInsideSelectionBounds(
            tile,
            visibilityContext);
    selection.priority = preparation.inputSummary.priority;

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
            context.queueTileLoad(
                tile.key,
                visitOutcome.loadGroup,
                preparation.inputSummary.priority);
        }
        return visitOutcome.returnCulledTraversalDetails
            ? context.createCulledTileDetails(tile)
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
    TileSelectionRasterOverlayPreparer::prepare(
        tile,
        context.rasterOverlays,
        context.device,
        context.options.maximumScreenSpaceError,
        context.frameResourceBudget);
    const TileSelectionFrameState& selection = tile.selectionFrameState;
    const bool renderable =
        !TileSelectionRootPolicy::isVirtualTerrainRoot(tile.key) &&
        TileSelectionRasterOverlayPreparer::isRenderable(
            tile,
            context.rasterOverlays);
    tile.updateTraversalRenderability(renderable);

    const bool tileCanRefine = context.canRefine(tile);
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
    if (preTraversal.queueUrgentLoad) {
        context.queueTileLoad(
            tile.key,
            TileLoadPriorityGroup::Urgent,
            tilePriority);
    }

    if (preTraversal.finishAsSingleTile) {
        context.addTileToCurrentPlan(
            tile,
            tileSse,
            preTraversal.singleTileShouldQueueLoad,
            tilePriority);
        return context.createSingleTileDetails(tile);
    }

    const TileChildFrameMaterializeResult childMaterialize =
        context.ensureTileChildren(tile);
    if (childMaterialize.retryLater) {
        context.queueTileLoad(
            tile.key,
            TileLoadPriorityGroup::Urgent,
            tilePriority);
        queuedForLoad = true;
    }

    if (preTraversal.addAdditiveParentToPlan) {
        context.addTileToCurrentPlan(
            tile,
            tileSse,
            preTraversal.additiveParentShouldQueueLoad,
            tilePriority);
    }

    const size_t firstRenderedDescendant =
        context.tilePlan.visibleTiles.size();
    const size_t loadQueueBeforeChildren = context.loadQueue.size();

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

    const TileSelectionPostTraversalResult postTraversal =
        TileSelectionPostTraversalPolicy::evaluate(
            TileSelectionPostTraversalInput{
                traversalDetails,
                renderable,
                tile.unconditionallyRefine,
                selection.previousSelectionState,
                context.hasLodTransitionRenderContent(tile),
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
                renderable},
            [](TilesetTile& kickedTile) {
                kickVisitedDescendants(kickedTile);
            },
            [&context](const TileKey& key,
                       TileLoadPriorityGroup group,
                       double priority) {
                context.queueTileLoad(key, group, priority);
            },
            [&context](TilesetTile& selectedTile,
                       double screenSpaceError,
                       bool queueForLoad,
                       double priority) {
                context.addTileToCurrentPlan(
                    selectedTile,
                    screenSpaceError,
                    queueForLoad,
                    priority);
            });

    if (commitResult.returnedSingleTileDetails) {
        return commitResult.details;
    }
    return traversalDetails;
}

} // namespace earth_engine
