#pragma once

#include "../scene/FrameState.h"

#include <cstdint>
#include <vector>

namespace earth_engine {

enum class TileSelectionReuseMode {
    None,
    Strict,
    Stale
};

enum class TileSelectionReuseRejectReason {
    None,
    NoReusableSelection,
    ViewportChanged,
    ResourceChanged,
    SelectorMovedStaleDisabled,
    StaleAgeExceeded,
    StaleViewTooDifferent,
    FadingTiles,
    PendingTilesetWork,
    PendingRasterOverlayWork,
    LastRequestIssuedWork,
    LastRequestBlockedByInflight
};

struct TileSelectionReuseClassification {
    TileSelectionReuseMode mode = TileSelectionReuseMode::None;
    TileSelectionReuseRejectReason rejectReason =
        TileSelectionReuseRejectReason::None;
};

struct TileSelectionReuseInput {
    const FrameState& frameState;
    const std::vector<FrameState::SelectorView>& lastSelectorViews;
    uint64_t currentResourceRevision = 0;
    uint64_t lastResourceRevision = 0;
    uint64_t currentOverlaySignature = 0;
    uint64_t lastOverlaySignature = 0;
    int lastViewportWidth = 0;
    int lastViewportHeight = 0;
    uint64_t currentFrameId = 0;
    uint64_t lastSelectionFrameId = 0;
    uint64_t maxStaleFrameAge = 1;
    double stalePositionToleranceMeters = 100.0;
    double staleDirectionToleranceSquared = 1e-4;
    bool hasReusableSelection = false;
    bool allowStaleSelection = false;
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

    static TileSelectionReuseMode classifyReuse(
        const TileSelectionReuseInput& input);
    static TileSelectionReuseClassification classifyReuseWithReason(
        const TileSelectionReuseInput& input);
    static bool canReuseSelection(const TileSelectionReuseInput& input);
};

} // namespace earth_engine
