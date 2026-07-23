#pragma once

#include "TileSelectionReusePolicy.h"

#include "../scene/FrameState.h"

#include <cstdint>
#include <vector>

namespace earth_engine {

struct TileSelectionReuseState {
    static constexpr uint64_t kDefaultMaxStaleFrameAge = 3;
    static constexpr double kDefaultStalePositionToleranceMeters = 100.0;
    static constexpr double kDefaultStaleDirectionToleranceSquared = 1e-4;

    uint64_t resourceRevision = 0;
    uint64_t overlaySignature = 0;
    int viewportWidth = 0;
    int viewportHeight = 0;
    uint64_t selectionFrameId = 0;
    bool reusable = false;
    bool lastRequestIssuedWork = false;
    bool lastRequestBlockedByInflight = false;
    std::vector<SelectorView> selectorViews;

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
        selectionFrameId = frameState.frameId;
        selectorViews = frameState.selectorViews;
    }

    void recordRequestOutcome(bool issuedWork, bool blockedByInflight) {
        lastRequestIssuedWork = issuedWork;
        lastRequestBlockedByInflight = blockedByInflight;
    }

    TileSelectionReuseMode classifyReuse(
        const FrameState& frameState,
        uint64_t currentResourceRevision,
        uint64_t currentOverlaySignature,
        bool allowStaleSelection,
        bool hasFadingTiles,
        bool hasPendingTilesetWork,
        bool hasPendingRasterOverlayWork,
        bool presentationHeld = false) const {
        return TileSelectionReusePolicy::classifyReuse(
            TileSelectionReuseInput{
                frameState,
                selectorViews,
                currentResourceRevision,
                resourceRevision,
                currentOverlaySignature,
                overlaySignature,
                viewportWidth,
                viewportHeight,
                frameState.frameId,
                selectionFrameId,
                kDefaultMaxStaleFrameAge,
                kDefaultStalePositionToleranceMeters,
                kDefaultStaleDirectionToleranceSquared,
                reusable,
                allowStaleSelection,
                hasFadingTiles,
                hasPendingTilesetWork,
                hasPendingRasterOverlayWork,
                lastRequestIssuedWork,
                lastRequestBlockedByInflight,
                presentationHeld});
    }

    TileSelectionReuseClassification classifyReuseWithReason(
        const FrameState& frameState,
        uint64_t currentResourceRevision,
        uint64_t currentOverlaySignature,
        bool allowStaleSelection,
        bool hasFadingTiles,
        bool hasPendingTilesetWork,
        bool hasPendingRasterOverlayWork,
        bool presentationHeld = false) const {
        return TileSelectionReusePolicy::classifyReuseWithReason(
            TileSelectionReuseInput{
                frameState,
                selectorViews,
                currentResourceRevision,
                resourceRevision,
                currentOverlaySignature,
                overlaySignature,
                viewportWidth,
                viewportHeight,
                frameState.frameId,
                selectionFrameId,
                kDefaultMaxStaleFrameAge,
                kDefaultStalePositionToleranceMeters,
                kDefaultStaleDirectionToleranceSquared,
                reusable,
                allowStaleSelection,
                hasFadingTiles,
                hasPendingTilesetWork,
                hasPendingRasterOverlayWork,
                lastRequestIssuedWork,
                lastRequestBlockedByInflight,
                presentationHeld});
    }

    bool canReuse(const FrameState& frameState,
                  uint64_t currentResourceRevision,
                  uint64_t currentOverlaySignature,
                  bool allowStaleSelection,
                  bool hasFadingTiles,
                  bool hasPendingTilesetWork,
                  bool hasPendingRasterOverlayWork) const {
        return classifyReuse(
                   frameState,
                   currentResourceRevision,
                   currentOverlaySignature,
                   allowStaleSelection,
                   hasFadingTiles,
                   hasPendingTilesetWork,
                   hasPendingRasterOverlayWork) != TileSelectionReuseMode::None;
    }
};

} // namespace earth_engine
