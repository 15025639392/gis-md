#include "MinimalGlobeDiagnostics.h"

#include "earth_engine/scene/PresentationTrace.h"

#include <gtest/gtest.h>

#include <string>

using namespace earth_engine;

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
    trace.tilesets.push_back(tileset);

    const std::string summary =
        minimal_globe_demo::buildPresentationTraceSummary(trace);

    EXPECT_NE(std::string::npos, summary.find("commands=1/3"));
    EXPECT_NE(std::string::npos, summary.find("missed=1"));
    EXPECT_NE(std::string::npos, summary.find("deferred=1"));
    EXPECT_NE(std::string::npos, summary.find("fallback=2"));
    EXPECT_NE(std::string::npos, summary.find("prepSync=3"));
    EXPECT_NE(std::string::npos, summary.find("prepDeferred=4"));
}
