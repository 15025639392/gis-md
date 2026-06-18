#pragma once

namespace earth_engine {

struct TileSelectionCounters {
    int visited = 0;
    int culled = 0;
    int kicked = 0;
    int fogCulled = 0;
    int notYetRenderable = 0;
    int culledVisited = 0;
    int occluded = 0;
    int waitingForOcclusionResults = 0;

    void reset() {
        visited = 0;
        culled = 0;
        kicked = 0;
        fogCulled = 0;
        notYetRenderable = 0;
        culledVisited = 0;
        occluded = 0;
        waitingForOcclusionResults = 0;
    }
};

} // namespace earth_engine
