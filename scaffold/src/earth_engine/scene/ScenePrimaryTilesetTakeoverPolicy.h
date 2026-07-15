#pragma once

namespace earth_engine {

class Tileset;

struct ScenePrimaryTilesetTakeoverState {
    int consecutiveReadyFrames = 0;
};

class ScenePrimaryTilesetTakeoverPolicy {
public:
    static bool isCandidateReady(
        const Tileset& currentPrimary,
        const Tileset& pendingPrimary);
    static bool shouldCommit(
        ScenePrimaryTilesetTakeoverState& state,
        const Tileset& currentPrimary,
        const Tileset& pendingPrimary);
};

} // namespace earth_engine
