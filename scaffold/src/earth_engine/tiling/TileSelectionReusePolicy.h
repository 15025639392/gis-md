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
    PendingTilesetWork,
    PendingRasterOverlayWork,
    LastRequestIssuedWork,
    LastRequestBlockedByInflight,
    PresentationHeld
};

struct TileSelectionReuseClassification {
    TileSelectionReuseMode mode = TileSelectionReuseMode::None;
    TileSelectionReuseRejectReason rejectReason =
        TileSelectionReuseRejectReason::None;
};

struct TileSelectionReuseInput {
    const FrameState& frameState;
    const std::vector<SelectorView>& lastSelectorViews;
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
    bool hasPendingTilesetWork = false;
    bool hasPendingRasterOverlayWork = false;
    bool lastRequestIssuedWork = false;
    bool lastRequestBlockedByInflight = false;
    // P5b:presentation hold(首屏 base 影像未达成)期间禁止 reuse。strict reuse
    // 跳过 overlay prefetch(映射推进+影像请求发起),其安全前提=「pending 工作会
    // 经 revision 变化打破 reuse」;但几何在单帧内全部 Done 时(GPU 位移 skip 让
    // upload 近零成本)revision 随即恒定 → 立即 reuse → 影像请求永不发出 → hold
    // 永不释放的启动死锁(与 TilesetUpdateFrameRuntime 顶部 async bootstrap
    // 死锁同型)。hold 仅启动/瞬态为真,不回退「泛泛 pending 阻断→静态反复
    // traverse」的历史权衡。
    bool presentationHeld = false;
};

class TileSelectionReusePolicy {
public:
    static bool selectorViewsEquivalent(
        const std::vector<SelectorView>& lhs,
        const std::vector<SelectorView>& rhs);

    static TileSelectionReuseMode classifyReuse(
        const TileSelectionReuseInput& input);
    static TileSelectionReuseClassification classifyReuseWithReason(
        const TileSelectionReuseInput& input);
    static bool canReuseSelection(const TileSelectionReuseInput& input);
};

} // namespace earth_engine
