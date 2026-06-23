#include "MinimalGlobeDiagnostics.h"

#include "earth_engine/scene/Diagnostics.h"
#include "earth_engine/scene/PresentationTrace.h"

#include <gtest/gtest.h>

#include <string>

using namespace earth_engine;

TEST(MinimalGlobeDiagnosticsTest, PanelReportsRenderEntryFunnelCounts) {
    Diagnostics diagnostics;
    diagnostics.terrainRenderEntriesPlanned = 9;
    diagnostics.terrainRenderEntriesSelectedPlanned = 7;
    diagnostics.terrainRenderEntriesFadingPlanned = 2;
    diagnostics.terrainRenderEntriesDrawn = 6;
    diagnostics.terrainRenderEntriesSelectedDrawn = 5;
    diagnostics.terrainRenderEntriesFadingDrawn = 1;
    diagnostics.terrainRenderEntriesMissed = 2;
    diagnostics.terrainRenderEntriesSelectedMissed = 1;
    diagnostics.terrainRenderEntriesFadingMissed = 1;
    diagnostics.terrainRenderEntriesDeferred = 1;
    diagnostics.terrainRenderEntriesSelectedDeferred = 1;
    diagnostics.terrainRenderEntriesFadingDeferred = 0;
    diagnostics.terrainRenderEntriesAncestorFallback = 3;
    diagnostics.terrainRenderEntriesSynchronousPrep = 4;
    diagnostics.terrainRenderEntriesDeferredPrep = 5;
    diagnostics.terrainSurfaceCommandsSubmitted = 6;
    diagnostics.globeFallbackCommands = 7;
    diagnostics.globeFallbackMaskedTerrainEntries = 8;

    const std::string line =
        minimal_globe_demo::buildRenderEntryDiagnosticsLine(diagnostics);

    EXPECT_NE(std::string::npos, line.find("plan 9 (sel 7, fade 2)"));
    EXPECT_NE(std::string::npos, line.find("draw 6 (sel 5, fade 1)"));
    EXPECT_NE(std::string::npos, line.find("miss 2 (sel 1, fade 1)"));
    EXPECT_NE(std::string::npos, line.find("defer 1 (sel 1, fade 0)"));
    EXPECT_NE(std::string::npos, line.find("fallback 3"));
    EXPECT_NE(std::string::npos, line.find("prep 4/5"));
    EXPECT_NE(std::string::npos, line.find("surface/globe/masked 6/7/8"));
}

TEST(MinimalGlobeDiagnosticsTest, SummaryReportsRenderEntryReasonCounts) {
    PresentationTrace trace;
    trace.camera.viewportWidthPixels = 800;
    trace.camera.viewportHeightPixels = 600;

    PresentationTilesetTrace tileset;
    tileset.visibleTiles.push_back(TileKey{"Geographic-TMS", 1, 0, 0});
    tileset.renderEntryPlannedCommandCount = 3;
    tileset.renderEntryCommandDrawCount = 1;
    tileset.renderEntryCommandMissedDrawCount = 1;
    tileset.renderEntryCommandDeferredCount = 1;
    tileset.renderEntryAncestorFallbackCount = 2;
    tileset.renderEntrySynchronousPrepCount = 3;
    tileset.renderEntryDeferredPrepCount = 4;
    tileset.minVisibleZoom = 1;
    tileset.maxVisibleZoom = 3;
    tileset.lodSizePixels = 16.0;
    tileset.frameMappedRasterTileCount = 5;
    tileset.frameMappedRasterTileLoadingCount = 2;
    tileset.frameProgressTotalCount = 8;
    tileset.frameProgressLoadingCount = 3;
    tileset.frameLoadProgressPercentage = 62.5;
    tileset.frameCredits = {"Imagery A", "Roads B", "Terrain C", "Extra D"};
    trace.tilesets.push_back(tileset);

    const std::string summary =
        minimal_globe_demo::buildPresentationTraceSummary(trace);

    EXPECT_NE(std::string::npos, summary.find("commands=1/3"));
    EXPECT_NE(std::string::npos, summary.find("missed=1"));
    EXPECT_NE(std::string::npos, summary.find("deferred=1"));
    EXPECT_NE(std::string::npos, summary.find("fallback=2"));
    EXPECT_NE(std::string::npos, summary.find("prepSync=3"));
    EXPECT_NE(std::string::npos, summary.find("prepDeferred=4"));
    EXPECT_NE(std::string::npos, summary.find("load=62%"));
    EXPECT_NE(std::string::npos, summary.find("work=3/8"));
    EXPECT_NE(std::string::npos, summary.find("mapped=2/5"));
    EXPECT_NE(
        std::string::npos,
        summary.find("Credits: Imagery A; Roads B; Terrain C; ..."));
}
