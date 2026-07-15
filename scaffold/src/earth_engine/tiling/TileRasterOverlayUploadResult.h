#pragma once

namespace earth_engine {

struct TileRasterOverlayUploadResult {
    int processedUploads = 0;
    int mappedUploads = 0;
    double maxUploadMs = 0.0;
    int maxUploadWidth = 0;
    int maxUploadHeight = 0;
    double selectTaskMs = 0.0;
    double uploadTextureMs = 0.0;
    double tileFinalizeMs = 0.0;
    double bookkeepingMs = 0.0;
    double sourceFallbackMs = 0.0;
    double sourceSnapshotMs = 0.0;
    double sourceIssueMs = 0.0;
    double uploadQueueSelectMs = 0.0;
    bool resourcesDirty = false;
};

} // namespace earth_engine
