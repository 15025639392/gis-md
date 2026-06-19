#pragma once

#include "TileLoadState.h"
#include "TilePlan.h"
#include "TileRenderContentState.h"
#include "TileSelectionFrameState.h"

namespace earth_engine {

class TileContentStateTransition {
public:
    static void markLoading(TileLoadState& loadState,
                            TileContentKind& contentKind) {
        loadState = TileLoadState::ContentLoading;
        contentKind = TileContentKind::Unknown;
    }

    static void markRenderLoaded(TileLoadState& loadState,
                                 TileContentKind& contentKind) {
        loadState = TileLoadState::ContentLoaded;
        contentKind = TileContentKind::Render;
    }

    static void markRenderDone(TileRenderContentState& renderContent,
                               TileLoadState& loadState,
                               TileContentKind& contentKind) {
        renderContent.setMeshReady(true);
        loadState = TileLoadState::Done;
        contentKind = TileContentKind::Render;
    }

    static void markRenderFailedTemporarily(
        TileRenderContentState& renderContent,
        TileLoadState& loadState,
        TileContentKind& contentKind) {
        renderContent.setMeshReady(false);
        contentKind = TileContentKind::Unknown;
        loadState = TileLoadState::FailedTemporarily;
    }

    static void markFailedTemporarily(TileLoadState& loadState,
                                      TileContentKind& contentKind) {
        contentKind = TileContentKind::Unknown;
        loadState = TileLoadState::FailedTemporarily;
    }

    static void markFailedPermanently(TileLoadState& loadState,
                                      TileContentKind& contentKind) {
        contentKind = TileContentKind::Unknown;
        loadState = TileLoadState::Failed;
    }

    static void markEmptyLoaded(TileLoadState& loadState,
                                TileContentKind& contentKind) {
        contentKind = TileContentKind::Empty;
        loadState = TileLoadState::ContentLoaded;
    }

    static void markEmptyDone(TileLoadState& loadState,
                              TileContentKind& contentKind) {
        contentKind = TileContentKind::Empty;
        loadState = TileLoadState::Done;
    }

    static void markExternalDone(TileLoadState& loadState,
                                 TileContentKind& contentKind,
                                 bool& unconditionallyRefine) {
        contentKind = TileContentKind::External;
        unconditionallyRefine = true;
        loadState = TileLoadState::Done;
    }

    static void markUnloading(TileLoadState& loadState) {
        loadState = TileLoadState::Unloading;
    }

    static void markUnloaded(TileLoadState& loadState,
                             TileContentKind& contentKind,
                             TileSelectionFrameState& selectionFrameState) {
        contentKind = TileContentKind::Unknown;
        loadState = TileLoadState::Unloaded;
        selectionFrameState.clearFrameRenderability();
    }
};

} // namespace earth_engine
