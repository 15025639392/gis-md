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

    const std::array<char, 640> detail =
        TileFrameDebugLogFormatter::updateDetail(input);
    const std::string text(detail.data());

    EXPECT_NE(text.find("reused=1"), std::string::npos);
    EXPECT_NE(text.find("reuseMode=2"), std::string::npos);
    EXPECT_NE(text.find("reuseReject=4"), std::string::npos);
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

    const std::array<char, 640> detail =
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
