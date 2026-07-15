#include <gtest/gtest.h>

#include "earth_engine/tiling/TileFrameDebugLogFormatter.h"

#include <array>
#include <string>

using namespace earth_engine;

TEST(TileFrameDebugLogFormatterTest, UpdateDetailReportsReuseMode) {
    TileUpdateDebugLogInput input;
    input.reusedSelection = true;
    input.reuseMode = TileSelectionReuseMode::Stale;
    input.reuseRejectReason =
        TileSelectionReuseRejectReason::SelectorMovedStaleDisabled;
    input.selectorTraversalMs = 12.5;
    input.selectorDetailedTimings = true;
    input.selectorVisitVisibilityMs = 2.5;
    input.selectorVisitInputMetricsMs = 3.5;
    input.selectorVisitPolicyMs = 0.25;
    input.selectorRefineMs = 4.25;
    input.selectorRefineOverlayMs = 0.75;
    input.selectorRefineDecisionMs = 1.25;
    input.selectorRefineMaterializeMs = 0.50;
    input.selectorRefineCommitMs = 1.75;
    input.selectorRefineMaterializeCalls = 12;
    input.selectorRefineMaterializeChanged = 2;
    input.selectorRefineMaterializeRetry = 1;
    input.selectorRefineMaterializeFastPath = 9;
    input.selectorRenderPlanMs = 2.0;
    input.selectorRequestPlanningMs = 1.5;
    input.rasterSelectTaskMs = 3.0;
    input.rasterUploadTextureMs = 5.0;
    input.rasterTileFinalizeMs = 7.0;
    input.rasterBookkeepingMs = 11.0;
    input.rasterSourceFallbackMs = 0.25;
    input.rasterSourceSnapshotMs = 0.50;
    input.rasterSourceIssueMs = 1.00;
    input.rasterUploadQueueSelectMs = 1.25;
    input.prefetchRenderPlanMs = 2.25;
    input.prefetchRenderPlanUpdateMs = 1.75;
    input.prefetchRenderPlanActionMs = 0.50;
    input.prefetchRenderPlanTiles = 40;
    input.prefetchRenderPlanAuthoritativeUpdates = 3;
    input.prefetchRenderPlanStableReuses = 37;
    input.gpuUploadDrainMs = 6.25;
    input.requestClassifiedContent = 11;
    input.requestClassifiedTerrainAvailabilityUpsample = 13;
    input.requestClassifiedRasterDetailUpsample = 17;
    input.requestIssuedContent = 2;
    input.requestIssuedTerrainAvailabilityUpsample = 3;
    input.requestIssuedRasterDetailUpsample = 5;
    input.requestUpsampleWorkerCapacity = 6;
    input.requestMotionDeferred = 7;

    const std::array<char, 1536> detail =
        TileFrameDebugLogFormatter::updateDetail(input);
    const std::string text(detail.data());

    EXPECT_NE(text.find("reused=1"), std::string::npos);
    EXPECT_NE(text.find("reuseMode=2"), std::string::npos);
    EXPECT_NE(text.find("reuseReject=4"), std::string::npos);
    EXPECT_NE(text.find("selTrav=12.50"), std::string::npos);
    EXPECT_NE(text.find("selDetail=1"), std::string::npos);
    EXPECT_NE(text.find("selVis=2.50"), std::string::npos);
    EXPECT_NE(text.find("selMetric=3.50"), std::string::npos);
    EXPECT_NE(text.find("selPolicy=0.25"), std::string::npos);
    EXPECT_NE(text.find("selRefine=4.25"), std::string::npos);
    EXPECT_NE(text.find("selOv=0.75"), std::string::npos);
    EXPECT_NE(text.find("selDec=1.25"), std::string::npos);
    EXPECT_NE(text.find("selMat=0.50"), std::string::npos);
    EXPECT_NE(text.find("selMatCalls=12"), std::string::npos);
    EXPECT_NE(text.find("selMatChanged=2"), std::string::npos);
    EXPECT_NE(text.find("selMatRetry=1"), std::string::npos);
    EXPECT_NE(text.find("selMatFast=9"), std::string::npos);
    EXPECT_NE(text.find("selCommit=1.75"), std::string::npos);
    EXPECT_NE(text.find("selPlan=2.00"), std::string::npos);
    EXPECT_NE(text.find("selReq=1.50"), std::string::npos);
    EXPECT_NE(text.find("rasterPick=3.00"), std::string::npos);
    EXPECT_NE(text.find("rasterFallback=0.25"), std::string::npos);
    EXPECT_NE(text.find("rasterSnapshot=0.50"), std::string::npos);
    EXPECT_NE(text.find("rasterIssue=1.00"), std::string::npos);
    EXPECT_NE(text.find("rasterQueue=1.25"), std::string::npos);
    EXPECT_NE(text.find("rasterTex=5.00"), std::string::npos);
    EXPECT_NE(text.find("rasterFinalize=7.00"), std::string::npos);
    EXPECT_NE(text.find("rasterBook=11.00"), std::string::npos);
    EXPECT_NE(text.find("prefRender=2.25"), std::string::npos);
    EXPECT_NE(text.find("prefRenderUpdate=1.75"), std::string::npos);
    EXPECT_NE(text.find("prefRenderAction=0.50"), std::string::npos);
    EXPECT_NE(text.find("prefRenderTiles=40"), std::string::npos);
    EXPECT_NE(text.find("prefRenderAuth=3"), std::string::npos);
    EXPECT_NE(text.find("prefRenderReuse=37"), std::string::npos);
    EXPECT_NE(text.find("reqClass=11/13/17"), std::string::npos);
    EXPECT_NE(text.find("reqIssued=2/3/5"), std::string::npos);
    EXPECT_NE(text.find("reqUpCap=6"), std::string::npos);
    EXPECT_NE(text.find("reqMotion=7"), std::string::npos);
    EXPECT_NE(text.find("gpuDrain=6.25"), std::string::npos);
}

TEST(
    TileFrameDebugLogFormatterTest,
    RenderBuildDetailReportsRenderEntryPassCounts) {
    TileRenderDebugLogInput input;
    input.renderStats.plannedEntries = 5;
    input.renderStats.drawAttempts = 3;
    input.commandCount = 3;
    input.selectedRenderStats.plannedEntries = 2;
    input.selectedRenderStats.drawAttempts = 1;
    input.selectedRenderStats.missedDrawEntries = 4;
    input.selectedRenderStats.deferredEntries = 6;
    input.fadingRenderStats.plannedEntries = 3;
    input.fadingRenderStats.drawAttempts = 2;
    input.fadingRenderStats.missedDrawEntries = 5;
    input.fadingRenderStats.deferredEntries = 7;
    input.renderStats.missedDrawEntries = 9;
    input.renderStats.deferredEntries = 13;

    const std::array<char, 1024> detail =
        TileFrameDebugLogFormatter::renderBuildDetail(input);
    const std::string text(detail.data());

    EXPECT_NE(text.find("entries=5"), std::string::npos);
    EXPECT_NE(text.find("selectedEntries=2"), std::string::npos);
    EXPECT_NE(text.find("fadeEntries=3"), std::string::npos);
    EXPECT_NE(text.find("cmds=3"), std::string::npos);
    EXPECT_NE(text.find("selectedCmds=1"), std::string::npos);
    EXPECT_NE(text.find("fadeCmds=2"), std::string::npos);
    EXPECT_NE(text.find("missed=9"), std::string::npos);
    EXPECT_NE(text.find("selectedMissed=4"), std::string::npos);
    EXPECT_NE(text.find("fadeMissed=5"), std::string::npos);
    EXPECT_NE(text.find("deferred=13"), std::string::npos);
    EXPECT_NE(text.find("selectedDeferred=6"), std::string::npos);
    EXPECT_NE(text.find("fadeDeferred=7"), std::string::npos);
}
