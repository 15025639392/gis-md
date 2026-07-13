#include "earth_engine/scene/SceneTelemetryCoordinator.h"

#include <gtest/gtest.h>

using namespace earth_engine;

TEST(SceneTelemetryCoordinatorTest, RetainsPeakRasterCpuBytesAcrossFrames) {
    SceneTelemetryCoordinator telemetry;

    Diagnostics first;
    first.rasterPendingUploadBytes = 12 * 1024;
    first.peakRasterPendingUploadBytes = 12 * 1024;
    first.rasterCachedSourceTileBytes = 34 * 1024;
    first.peakRasterCachedSourceTileBytes = 34 * 1024;
    telemetry.replaceRenderDiagnostics(first);

    Diagnostics second;
    second.rasterPendingUploadBytes = 2 * 1024;
    second.peakRasterPendingUploadBytes = 2 * 1024;
    second.rasterCachedSourceTileBytes = 3 * 1024;
    second.peakRasterCachedSourceTileBytes = 3 * 1024;
    telemetry.replaceRenderDiagnostics(second);

    const Diagnostics& diagnostics = telemetry.diagnostics();
    EXPECT_EQ(diagnostics.rasterPendingUploadBytes, 2 * 1024);
    EXPECT_EQ(diagnostics.rasterCachedSourceTileBytes, 3 * 1024);
    EXPECT_EQ(diagnostics.peakRasterPendingUploadBytes, 12 * 1024);
    EXPECT_EQ(diagnostics.peakRasterCachedSourceTileBytes, 34 * 1024);
}
