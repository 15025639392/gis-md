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

bool TileSelectionReusePolicy::canReuseSelection(
    const TileSelectionReuseInput& input) {
    if (!input.hasReusableSelection) {
        return false;
    }
    if (input.frameState.viewportWidthPixels != input.lastViewportWidth ||
        input.frameState.viewportHeightPixels != input.lastViewportHeight) {
        return false;
    }
    if (input.currentResourceRevision != input.lastResourceRevision ||
        input.currentOverlaySignature != input.lastOverlaySignature) {
        return false;
    }
    if (!selectorViewsEquivalent(
            input.frameState.selectorViews,
            input.lastSelectorViews)) {
        return false;
    }
    if (input.hasFadingTiles) {
        return false;
    }
    if (input.hasPendingTilesetWork || input.hasPendingRasterOverlayWork) {
        return false;
    }
    if (input.lastRequestIssuedWork || input.lastRequestBlockedByInflight) {
        return false;
    }
    return true;
}

} // namespace earth_engine
