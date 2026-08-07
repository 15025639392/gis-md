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
    const std::vector<SelectorView>& lhs,
    const std::vector<SelectorView>& rhs,
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
    const std::vector<SelectorView>& lhs,
    const std::vector<SelectorView>& rhs) {
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
    // P5b:presentation hold 期间不 reuse——strict reuse 跳过 overlay prefetch,
    // 若此时首屏 base 影像尚未达成,影像请求可能还没发出,reuse 会把加载链
    // 冻死(黑屏死锁,详见 TileSelectionReuseInput::presentationHeld 注释)。
    if (input.presentationHeld) {
        return {
            TileSelectionReuseMode::None,
            TileSelectionReuseRejectReason::PresentationHeld};
    }
    if (input.frameState.viewportWidthPixels != input.lastViewportWidth ||
        input.frameState.viewportHeightPixels != input.lastViewportHeight) {
        return {
            TileSelectionReuseMode::None,
            TileSelectionReuseRejectReason::ViewportChanged};
    }
    if (input.currentOverlaySignature != input.lastOverlaySignature) {
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
    if (input.currentResourceRevision != input.lastResourceRevision) {
        return {
            TileSelectionReuseMode::None,
            TileSelectionReuseRejectReason::ResourceChanged};
    }
    // Equivalent views can keep drawing the committed selection while pending
    // network/upload work drains. Completed terrain/content resources still
    // invalidate reuse through the resource revision; raster uploads are
    // attached during render preparation. Treating every pending item as a
    // strict-reuse blocker makes static Android loading repeatedly traverse and
    // prefetch the same large tile set.
    return {
        TileSelectionReuseMode::Strict,
        TileSelectionReuseRejectReason::None};
}

bool TileSelectionReusePolicy::canReuseSelection(
    const TileSelectionReuseInput& input) {
    return classifyReuse(input) != TileSelectionReuseMode::None;
}

} // namespace earth_engine
