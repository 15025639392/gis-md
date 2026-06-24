#pragma once

#include "TileLoadState.h"

namespace earth_engine {

struct TileLoadStatePredicates {
    static bool canRefreshContentMetadataBeforeContentAccepted(
        TileLoadState state) {
        switch (state) {
        case TileLoadState::Unloaded:
        case TileLoadState::ContentLoading:
            return true;
        case TileLoadState::Unloading:
        case TileLoadState::FailedTemporarily:
        case TileLoadState::ContentLoaded:
        case TileLoadState::Done:
        case TileLoadState::Failed:
            return false;
        }
        return false;
    }

    static bool hasResolvedAvailabilityBoundaryContent(
        TileLoadState state) {
        return state > TileLoadState::ContentLoading;
    }
};

} // namespace earth_engine
