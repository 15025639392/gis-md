#pragma once

#include "TileLoadState.h"

namespace earth_engine {

struct TileLoadStatePredicates {
    static bool hasResolvedAvailabilityBoundaryContent(
        TileLoadState state) {
        switch (state) {
        case TileLoadState::ContentLoaded:
        case TileLoadState::Done:
        case TileLoadState::Failed:
            return true;
        case TileLoadState::Unloading:
        case TileLoadState::FailedTemporarily:
        case TileLoadState::Unloaded:
        case TileLoadState::ContentLoading:
            return false;
        }
        return false;
    }
};

} // namespace earth_engine
