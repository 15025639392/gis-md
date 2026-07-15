#pragma once

namespace earth_engine {

struct TileSelectionPerformanceTimings {
    bool collectDetailed = false;
    double traversalMs = 0.0;
    double refineMs = 0.0;
    double renderPlanMs = 0.0;
    double refineOverlayMs = 0.0;
    double refineDecisionMs = 0.0;
    double refineMaterializeMs = 0.0;
    double refineCommitMs = 0.0;
    double visitVisibilityMs = 0.0;
    double visitInputMetricsMs = 0.0;
    double visitPolicyMs = 0.0;
    int refineMaterializeCalls = 0;
    int refineMaterializeChanged = 0;
    int refineMaterializeRetry = 0;
    int refineMaterializeFastPath = 0;
};

} // namespace earth_engine
