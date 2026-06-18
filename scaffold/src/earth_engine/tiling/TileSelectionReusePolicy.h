#pragma once

#include "../scene/FrameState.h"

#include <cstdint>
#include <vector>

namespace earth_engine {

struct TileSelectionReuseInput {
    const FrameState& frameState;
    const std::vector<FrameState::SelectorView>& lastSelectorViews;
    uint64_t currentResourceRevision = 0;
    uint64_t lastResourceRevision = 0;
    uint64_t currentOverlaySignature = 0;
    uint64_t lastOverlaySignature = 0;
    int lastViewportWidth = 0;
    int lastViewportHeight = 0;
    bool hasReusableSelection = false;
    bool hasFadingTiles = false;
    bool hasPendingTilesetWork = false;
    bool hasPendingRasterOverlayWork = false;
    bool lastRequestIssuedWork = false;
    bool lastRequestBlockedByInflight = false;
};

class TileSelectionReusePolicy {
public:
    static bool selectorViewsEquivalent(
        const std::vector<FrameState::SelectorView>& lhs,
        const std::vector<FrameState::SelectorView>& rhs);

    static bool canReuseSelection(const TileSelectionReuseInput& input);
};

} // namespace earth_engine
