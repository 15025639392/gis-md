#pragma once

namespace earth_engine {

enum class TileLoadState {
    Unloading = -2,
    FailedTemporarily = -1,
    Unloaded = 0,
    ContentLoading = 1,
    ContentLoaded = 2,
    Done = 3,
    Failed = 4
};

enum class TileContentKind {
    Unknown,
    Empty,
    External,
    Render
};

} // namespace earth_engine
