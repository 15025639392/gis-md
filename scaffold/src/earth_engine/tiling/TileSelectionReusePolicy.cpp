#include "TileSelectionReusePolicy.h"

#include <cmath>

namespace earth_engine {
namespace {

bool matricesNearlyEqual(const Mat4& lhs,
                         const Mat4& rhs,
                         double epsilon) {
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            if (std::abs(lhs(row, col) - rhs(row, col)) > epsilon) {
                return false;
            }
        }
    }
    return true;
}

bool selectorViewsStaleCompatible(
    const std::vector<FrameState::SelectorView>& lhs,
    const std::vector<FrameState::SelectorView>& rhs,
    double positionToleranceMeters,
    double directionToleranceSquared) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        const auto& a = lhs[i];
        const auto& b = rhs[i];
        if (a.position.distanceTo(b.position) > positionToleranceMeters) {
            return false;
        }
        if ((a.direction - b.direction).lengthSquared() >
            directionToleranceSquared) {
            return false;
        }
        if (!matricesNearlyEqual(
                a.projectionMatrix,
                b.projectionMatrix,
                1e-12)) {
            return false;
        }
        if (a.viewportHeightPixels != b.viewportHeightPixels) {
            return false;
        }
    }
    return true;
}

} // namespace

bool TileSelectionReusePolicy::selectorViewsEquivalent(
    const std::vector<FrameState::SelectorView>& lhs,
    const std::vector<FrameState::SelectorView>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        const auto& a = lhs[i];
        const auto& b = rhs[i];
        if (a.position.distanceTo(b.position) > 1e-3) {
            return false;
        }
        if ((a.direction - b.direction).lengthSquared() > 1e-12) {
            return false;
        }
        if (!matricesNearlyEqual(
                a.projectionMatrix,
                b.projectionMatrix,
                1e-12)) {
            return false;
        }
        if (a.viewportHeightPixels != b.viewportHeightPixels) {
            return false;
        }
    }
    return true;
}

TileSelectionReuseMode TileSelectionReusePolicy::classifyReuse(
    const TileSelectionReuseInput& input) {
    return classifyReuseWithReason(input).mode;
}

TileSelectionReuseClassification
TileSelectionReusePolicy::classifyReuseWithReason(
    const TileSelectionReuseInput& input) {
    if (!input.hasReusableSelection) {
        return {
            TileSelectionReuseMode::None,
            TileSelectionReuseRejectReason::NoReusableSelection};
    }
    if (input.frameState.viewportWidthPixels != input.lastViewportWidth ||
        input.frameState.viewportHeightPixels != input.lastViewportHeight) {
        return {
            TileSelectionReuseMode::None,
            TileSelectionReuseRejectReason::ViewportChanged};
    }
    if (input.currentResourceRevision != input.lastResourceRevision ||
        input.currentOverlaySignature != input.lastOverlaySignature) {
        return {
            TileSelectionReuseMode::None,
            TileSelectionReuseRejectReason::ResourceChanged};
    }
    if (!selectorViewsEquivalent(
            input.frameState.selectorViews,
            input.lastSelectorViews)) {
        if (!input.allowStaleSelection) {
            return {
                TileSelectionReuseMode::None,
                TileSelectionReuseRejectReason::SelectorMovedStaleDisabled};
        }
        if (input.currentFrameId < input.lastSelectionFrameId ||
            input.currentFrameId - input.lastSelectionFrameId >
                input.maxStaleFrameAge) {
            return {
                TileSelectionReuseMode::None,
                TileSelectionReuseRejectReason::StaleAgeExceeded};
        }
        if (!selectorViewsStaleCompatible(
                input.frameState.selectorViews,
                input.lastSelectorViews,
                input.stalePositionToleranceMeters,
                input.staleDirectionToleranceSquared)) {
            return {
                TileSelectionReuseMode::None,
                TileSelectionReuseRejectReason::StaleViewTooDifferent};
        }
        return {
            TileSelectionReuseMode::Stale,
            TileSelectionReuseRejectReason::None};
    }
    if (input.hasFadingTiles) {
        return {
            TileSelectionReuseMode::None,
            TileSelectionReuseRejectReason::FadingTiles};
    }
    if (input.hasPendingTilesetWork) {
        return {
            TileSelectionReuseMode::None,
            TileSelectionReuseRejectReason::PendingTilesetWork};
    }
    if (input.hasPendingRasterOverlayWork) {
        return {
            TileSelectionReuseMode::None,
            TileSelectionReuseRejectReason::PendingRasterOverlayWork};
    }
    if (input.lastRequestIssuedWork) {
        return {
            TileSelectionReuseMode::None,
            TileSelectionReuseRejectReason::LastRequestIssuedWork};
    }
    if (input.lastRequestBlockedByInflight) {
        return {
            TileSelectionReuseMode::None,
            TileSelectionReuseRejectReason::LastRequestBlockedByInflight};
    }
    return {
        TileSelectionReuseMode::Strict,
        TileSelectionReuseRejectReason::None};
}

bool TileSelectionReusePolicy::canReuseSelection(
    const TileSelectionReuseInput& input) {
    return classifyReuse(input) != TileSelectionReuseMode::None;
}

} // namespace earth_engine
