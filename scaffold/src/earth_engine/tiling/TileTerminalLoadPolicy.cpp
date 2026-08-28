#include "TileTerminalLoadPolicy.h"

#include "DirectRasterMapping.h"
#include "TileLoadTypes.h"
#include "TilesetTile.h"
#include "../debug/PerfTimer.h"

namespace earth_engine {

namespace {

void markUnknownTemporaryFailure(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer,
    bool accrueBackoff = true) {
    tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
    if (tile.content.contentKind == TileContentKind::Render) {
        tile.content.loadState = TileLoadState::FailedTemporarily;
        tile.syncContentLoadWorkTicket();  // 直写 loadState,绕过 mark* 家族
        tile.notifyChildMaterializationStateChanged();
    } else {
        tile.markContentFailedTemporarily();
    }
    // 指数退避:避免 FailedTemporarily 瓦片每帧重打服务器(见
    // TileRetryBackoffPolicy);成功加载时在 TilesetTile 标记器里重置。
    //
    // ⚠️**取消不累加退避**。退避是给"源在抖/在限流"用的,而取消是**我们
    // 自己**掐的(stale 差集回收),源什么错都没有。把它按失败计数,冷启动
    // 会自我惩罚:一批在飞请求被清扫 → 全体退避翻倍 → 下一轮更慢 → 更容易
    // 再次过龄被清扫。真机 2026-08-09(25000m 冷启动)实测:计入退避时 50s
    // 内只加载到 8~16 块瓦片,而同场景另一次跑到 136 块 —— 方差就来自退避
    // 阶梯爬到了哪一级。
    if (accrueBackoff) {
        tile.recordTemporaryFailureBackoff(perf::nowMs());
    }
}

void markUnknownPermanentFailure(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer) {
    tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
    tile.content.renderContent.clearRenderContent();
    tile.markContentFailedPermanently();
}

void clearRenderResidueForTerminalNonRenderContent(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer) {
    tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
    tile.content.renderContent.clearRenderContent();
}

void applyNativeEmptyContentRefinement(TilesetTile& tile) {
    const TilesetTile* ancestor = tile.parent;
    while (ancestor && ancestor->unconditionallyRefine) {
        ancestor = ancestor->parent;
    }
    const double tileError = tile.nonZeroGeometricError();
    const double parentError = ancestor
        ? ancestor->nonZeroGeometricError()
        : tileError * 2.0;
    if (tileError >= parentError) {
        tile.setUnconditionallyRefine(true);
    }
}

TileTerminalLoadAction applyTerminalResult(
    TilesetTile& tile,
    TileLoadStatus status,
    IPrepareRendererResources* pPrepRenderer,
    bool allowExternalContent) {
    TileTerminalLoadAction action;

    switch (status) {
        case TileLoadStatus::Empty:
            action.markEmptyCacheKey = true;
            clearRenderResidueForTerminalNonRenderContent(
                tile,
                pPrepRenderer);
            applyNativeEmptyContentRefinement(tile);
            tile.markEmptyContentDone();
            action.ensureChildren = true;
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::External:
            if (allowExternalContent) {
                clearRenderResidueForTerminalNonRenderContent(
                    tile,
                    pPrepRenderer);
                tile.markExternalContentDone();
                action.ensureChildren = true;
            } else {
                markUnknownPermanentFailure(tile, pPrepRenderer);
            }
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::RetryLater:
            markUnknownTemporaryFailure(tile, pPrepRenderer);
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::Cancelled:
            // 见 markUnknownTemporaryFailure 里那段:取消是自伤,不计退避。
            markUnknownTemporaryFailure(tile, pPrepRenderer, false);
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::Failed:
        case TileLoadStatus::Renderable:
            markUnknownPermanentFailure(tile, pPrepRenderer);
            action.resourcesDirty = true;
            break;
    }

    return action;
}

} // namespace

TileTerminalLoadAction
TileTerminalLoadPolicy::applyTerminalResult(
    TileLoadDomain domain,
    TilesetTile& tile,
    TileLoadStatus status,
    IPrepareRendererResources* pPrepRenderer) {
    return ::earth_engine::applyTerminalResult(
        tile,
        status,
        pPrepRenderer,
        domain == TileLoadDomain::Content);
}

} // namespace earth_engine
