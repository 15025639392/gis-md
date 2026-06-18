#pragma once

#include "TileSelectionReusePolicy.h"

#include "../scene/FrameState.h"

#include <cstdint>
#include <vector>

namespace earth_engine {

struct TileSelectionReuseState {
    uint64_t resourceRevision = 0;
    uint64_t overlaySignature = 0;
    int viewportWidth = 0;
    int viewportHeight = 0;
    bool reusable = false;
    bool lastRequestIssuedWork = false;
    bool lastRequestBlockedByInflight = false;
    std::vector<FrameState::SelectorView> selectorViews;

    void invalidate() {
        reusable = false;
    }

    void commit(const FrameState& frameState,
                uint64_t currentResourceRevision,
                uint64_t currentOverlaySignature) {
        reusable = true;
        resourceRevision = currentResourceRevision;
        overlaySignature = currentOverlaySignature;
        viewportWidth = frameState.viewportWidthPixels;
        viewportHeight = frameState.viewportHeightPixels;
        selectorViews = frameState.selectorViews;
    }

    void recordRequestOutcome(bool issuedWork, bool blockedByInflight) {
        lastRequestIssuedWork = issuedWork;
        lastRequestBlockedByInflight = blockedByInflight;
    }

    bool canReuse(const FrameState& frameState,
                  uint64_t currentResourceRevision,
                  uint64_t currentOverlaySignature,
                  bool hasFadingTiles,
                  bool hasPendingTilesetWork,
                  bool hasPendingRasterOverlayWork) const {
        return TileSelectionReusePolicy::canReuseSelection(
            TileSelectionReuseInput{
                frameState,
                selectorViews,
                currentResourceRevision,
                resourceRevision,
                currentOverlaySignature,
                overlaySignature,
                viewportWidth,
                viewportHeight,
                reusable,
                hasFadingTiles,
                hasPendingTilesetWork,
                hasPendingRasterOverlayWork,
                lastRequestIssuedWork,
                lastRequestBlockedByInflight});
    }
};

} // namespace earth_engine
