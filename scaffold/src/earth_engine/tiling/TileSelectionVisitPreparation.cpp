#include "TileSelectionVisitPreparation.h"

#include "RasterMappedToTilesetTile.h"
#include "TileViewerRequestVolumePolicy.h"
#include "TilesetTile.h"
#include "../debug/PerfTimer.h"

namespace earth_engine {

TileSelectionVisitPreparationResult TileSelectionVisitPreparation::prepare(
    const TilesetTile& tile,
    const std::vector<SelectorView>& views,
    const std::vector<double>& fogDensities,
    const TileSelectionVisibilityContext& visibilityContext,
    const TileSelectionVisitPreparationOptions& options,
    std::vector<double>& scratchDistances) {
    TileSelectionVisitPreparationResult result;
    const double visibilityStartMs =
        options.collectDetailedTimings ? perf::nowMs() : 0.0;
    result.visibilitySample =
        TileSelectionVisibilitySampler::sampleForTileSelection(
            tile,
            views,
            visibilityContext);
    if (options.collectDetailedTimings) {
        result.visibilityMs = perf::nowMs() - visibilityStartMs;
    }

    const double inputMetricsStartMs =
        options.collectDetailedTimings ? perf::nowMs() : 0.0;
    result.inputSummary =
        TileSelectionInputMetrics::summarizeForViews(
            tile,
            views,
            scratchDistances);
    if (options.collectDetailedTimings) {
        result.inputMetricsMs = perf::nowMs() - inputMetricsStartMs;
    }

    const double policyStartMs =
        options.collectDetailedTimings ? perf::nowMs() : 0.0;
    result.cullResult = TileSelectionCullingPolicy::evaluateFrustum(
        result.visibilitySample.visibleFromCamera,
        options.enableFrustumCulling);
    result.cullResult = TileSelectionCullingPolicy::evaluateFog(
        result.cullResult,
        TileSelectionCullingPolicy::anyViewVisibleInFog(
            scratchDistances,
            fogDensities),
        options.enableFogCulling);
    if (!result.cullResult.shouldVisit &&
        tile.unconditionallyRefine &&
        ((options.forbidHoles && tile.refine == TileRefine::Replace) ||
         tile.parent == nullptr)) {
        result.cullResult.shouldVisit = true;
    }
    result.culledLoadPlan = TileSelectionCullingPolicy::planCulledTileLoad(
        options.preloadSiblings,
        options.forbidHoles,
        tile.refine);
    result.viewerRequestVolumeAllowed =
        TileViewerRequestVolumePolicy::allowsAnyView(
            tile.viewerRequestVolume,
            views);
    result.meetsScreenSpaceError =
        TileSelectionCullingPolicy::meetsScreenSpaceError(
            result.cullResult.culled,
            result.inputSummary.screenSpaceError,
            options.maximumScreenSpaceError,
            options.enforceCulledScreenSpaceError,
            options.culledScreenSpaceError);
    if (options.collectDetailedTimings) {
        result.policyMs = perf::nowMs() - policyStartMs;
    }
    return result;
}

TileSelectionVisitEarlyExitPlan
TileSelectionVisitPreparation::earlyExitPlan(
    const TileSelectionVisitPreparationResult& preparation) {
    TileSelectionVisitEarlyExitPlan plan;
    if (!preparation.cullResult.shouldVisit) {
        plan.shouldExit = true;
        plan.reason = TileSelectionVisitEarlyExitReason::Culled;
        plan.markTileCulled = true;
        plan.queueCulledLoad = preparation.culledLoadPlan.queueLoad;
        plan.loadGroup = preparation.culledLoadPlan.group;
        plan.counter =
            preparation.cullResult.reason == TileSelectionCullReason::Frustum
            ? TileSelectionVisitEarlyExitCounter::FrustumCulled
            : TileSelectionVisitEarlyExitCounter::FogCulled;
        return plan;
    }

    if (!preparation.viewerRequestVolumeAllowed) {
        plan.shouldExit = true;
        plan.reason = TileSelectionVisitEarlyExitReason::ViewerRequestVolume;
        plan.markTileCulled = true;
        plan.resetScreenSpaceError = true;
        return plan;
    }

    return plan;
}

TileSelectionVisitOutcomePlan TileSelectionVisitPreparation::outcomePlan(
    const TileSelectionVisitPreparationResult& preparation) {
    TileSelectionVisitOutcomePlan outcome;
    const TileSelectionVisitEarlyExitPlan earlyExit =
        earlyExitPlan(preparation);

    if (earlyExit.shouldExit) {
        outcome.shouldExit = true;
        outcome.exitReason = earlyExit.reason;
        outcome.counter = earlyExit.counter;
        outcome.markTileCulled = earlyExit.markTileCulled;
        outcome.resetScreenSpaceError = earlyExit.resetScreenSpaceError;
        outcome.queueLoad = earlyExit.queueCulledLoad;
        outcome.loadGroup = earlyExit.loadGroup;
        outcome.returnCulledTraversalDetails =
            earlyExit.reason == TileSelectionVisitEarlyExitReason::Culled;
        return outcome;
    }

    outcome.countCulledVisited = preparation.cullResult.culled;
    return outcome;
}

} // namespace earth_engine
