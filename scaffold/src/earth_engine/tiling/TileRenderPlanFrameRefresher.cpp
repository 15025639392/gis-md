#include "TileRenderPlanFrameRefresher.h"

#include "TileCacheKey.h"
#include "TileContentAccess.h"
#include "RasterMappedToTilesetTile.h"
#include "TileRasterOverlayReadinessPolicy.h"
#include "TileRenderPlanFinalizer.h"
#include "TileEdgeMismatchProbe.h"
#include "TileEdgeSnapResolver.h"
#include "TilesetTile.h"
#include "../debug/PlatformLog.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/ImageryProvider.h"
#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"

#include <unordered_set>
#include <vector>

namespace earth_engine {

namespace {

constexpr int kActiveInteractionRenderPrepBudget = 0;
constexpr int kRecoveryRenderPrepBudget = 1;

// 接边错位的报告周期(帧)。**逐帧测量、窗口累积、周期报告**:单帧只有 1~2
// 条 fade 一致的吸附边(n≈17),这个样本量撑不起 A/B 判定;而逐帧累积一秒就是
// 60 倍样本,还天然覆盖整段运动(LOD 边界最丰富的地方恰恰在运动期)。
// 逐帧成本:掠视稳态约 10 条边 × 33 节点 × 3 次双线性 ≈ 1000 次/帧,可忽略。
constexpr uint64_t kEdgeMismatchLogPeriod = 60;

/// 把一个群体的分布写成一段文本。末桶是"大于最大有限上界"的溢出桶,打它的
/// 哨兵值(1e30)只会刷屏 —— 写成 >25m。
void formatEdgeStats(const TileEdgeMismatchProbe::Stats& st, const char* tag,
                     char* buf, size_t cap) {
    if (!st.hasSamples()) {
        snprintf(buf, cap, "%s=无样本", tag);
        return;
    }
    const float p95 = st.percentileUpper(0.95);
    const float lastFinite = TileEdgeMismatchProbe::kBucketUpper
        [TileEdgeMismatchProbe::kBucketCount - 2];
    char p95Text[16];
    if (p95 > lastFinite) {
        snprintf(p95Text, sizeof(p95Text), ">%.0fm", lastFinite);
    } else {
        snprintf(p95Text, sizeof(p95Text), "<=%.1fm", p95);
    }
    snprintf(buf, cap,
             "%s{edges=%d n=%d agree=%.3f mean=%.2fm p95%s max=%.2fm}",
             tag, st.edges, st.samples, st.agreementRatio(), st.meanAbs(),
             p95Text, static_cast<double>(st.maxAbs));
}

/// 逐帧累积接边错位,每 kEdgeMismatchLogPeriod 帧报一次并清窗。
///
/// ⚠️ 窗口累积器是**文件级静态**:诊断专用,多 tileset 时会合并成一份。给它
/// 找个正经宿主要么污染 TilePlan(每帧对象,语义不符)要么给 refresher 加实例
/// 态(它现在是纯静态类),为一条诊断付这个结构代价不划算。demo 单 tileset。
void logEdgeMismatch(const TilePlan& plan) {
    static TileEdgeMismatchProbe::Result window;
    window.merge(TileEdgeMismatchProbe::measure(plan));
    if (plan.frameId == 0 || plan.frameId % kEdgeMismatchLogPeriod != 0) {
        return;
    }
    // 两个群体都没样本 = 本窗口没有吸附边(俯视/单档场景),**不打印**。
    // 打一行全零会和"测了、确实是零"长得一模一样。
    if (!window.fadeUniform.hasSamples() && !window.fadeDiffer.hasSamples()) {
        if (window.skippedEdges > 0) {
            platformLog(LogLevel::Info, "SeamDiag",
                        "edgeMismatch frame=%llu 无样本 skipped=%d"
                        "(两侧 heightmap 取不全)",
                        static_cast<unsigned long long>(plan.frameId),
                        window.skippedEdges);
        }
        window = TileEdgeMismatchProbe::Result{};
        return;
    }
    char uniformText[160];
    char differText[160];
    char compText[160];
    // fadeUniform 是 ①-1 的目标群体,A/B 只看它;fadeDiffer 是设计使然的
    // 压平台阶,列出来只为佐证"大数字来自它、不是 ε"。
    formatEdgeStats(window.fadeUniform, "fadeUniform", uniformText,
                    sizeof(uniformText));
    formatEdgeStats(window.fadeDiffer, "fadeDiffer", differText,
                    sizeof(differText));
    // compensated 才是「①-1 之后还剩多少」的读数;fadeUniform 是补偿**前**的
    // 原始 ε(它的样本值就是 LUT delta 的定义式,拿它判残余会系统性高估)。
    formatEdgeStats(window.compensated, "compensated", compText,
                    sizeof(compText));
    platformLog(LogLevel::Info, "SeamDiag",
                "edgeMismatch frame=%llu win=%llu %s %s %s skipped=%d",
                static_cast<unsigned long long>(plan.frameId),
                static_cast<unsigned long long>(kEdgeMismatchLogPeriod),
                uniformText, differText, compText, window.skippedEdges);
    // fadeDiffer 的可接受性:台阶/裙墙。overSkirt=0 → 台阶全被裙墙盖住,
    // 「设计使然」这个说法成立;>0 → 那些是透天洞,不是设计。
    if (window.fadeDifferSamples > 0) {
        platformLog(LogLevel::Info, "SeamDiag",
                    "  fadeDifferSkirt n=%d overSkirt=%d fineAbove=%d "
                    "maxRatio=%.2f skirt=%.1fm",
                    window.fadeDifferSamples, window.fadeDifferOverSkirt,
                    window.fadeDifferFineAbove,
                    static_cast<double>(window.fadeDifferMaxRatio),
                    window.fadeDifferSkirtMeters);
    }
    window = TileEdgeMismatchProbe::Result{};
}

// 去重集合 + 输出 vector 并行维护：vector 保持插入序（展示顺序稳定），
// set 提供 O(1) 查重——原先逐条 std::find 在瓦片×credit 数上是 O(N²)
// 字符串比较（P2-10）。
struct FrameCreditCollector {
    std::vector<std::string>& frameCredits;
    std::unordered_set<std::string> seen;

    void append(const std::string& credit) {
        if (credit.empty()) {
            return;
        }
        if (seen.insert(credit).second) {
            frameCredits.push_back(credit);
        }
    }
};

void collectReadyRasterTileCredits(const TilesetTile& tile,
                                   FrameCreditCollector& credits) {
    tile.rasterOverlayState.forEachMapping(
        [&credits](const RasterMappedToTilesetTile* mapping) {
            if (!mapping) {
                return;
            }

            const RasterOverlayTile* readyTile = mapping->getReadyTile();
            if (!readyTile) {
                return;
            }

            for (const std::string& credit : readyTile->credits()) {
                credits.append(credit);
            }
        });
}

void collectRenderContentCredits(const TilesetTile& tile,
                                 FrameCreditCollector& credits) {
    for (const std::string& credit : tile.content.renderContent.credits()) {
        credits.append(credit);
    }
}

void collectRasterOverlayProviderCredits(
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    FrameCreditCollector& credits) {
    for (const ActivatedRasterOverlay* overlay : rasterOverlays) {
        if (!overlay) {
            continue;
        }

        const RasterOverlayTileProvider* tileProvider =
            overlay->getTileProvider();
        if (!tileProvider) {
            continue;
        }

        credits.append(tileProvider->getImageryProvider().attribution());
    }
}

void refreshFrameCredits(TilePlan& tilePlan,
                         const std::vector<ActivatedRasterOverlay*>&
                             rasterOverlays) {
    tilePlan.frameCredits.clear();
    FrameCreditCollector credits{tilePlan.frameCredits, {}};
    collectRasterOverlayProviderCredits(rasterOverlays, credits);

    for (const TileRenderEntry& entry : tilePlan.renderEntries) {
        if (entry.selectedTile) {
            collectRenderContentCredits(*entry.selectedTile, credits);
            collectReadyRasterTileCredits(*entry.selectedTile, credits);
        }

        if (entry.renderTile &&
            entry.renderTile != entry.selectedTile) {
            collectRenderContentCredits(*entry.renderTile, credits);
            collectReadyRasterTileCredits(*entry.renderTile, credits);
        }
    }
}

void collectMappedRasterProgress(const TilesetTile& tile,
                                 TilePlan& tilePlan) {
    tile.rasterOverlayState.forEachMapping(
        [&tilePlan](const RasterMappedToTilesetTile* mapping) {
            if (!mapping) {
                return;
            }

            ++tilePlan.frameMappedRasterTileCount;
            if (mapping->getState() !=
                RasterMappedToTilesetTile::State::Attached) {
                ++tilePlan.frameMappedRasterTileLoadingCount;
            }
        });
}

int baseProgressTotalCount(const TilePlan& tilePlan) {
    const int selectedTraversalCount =
        tilePlan.renderingNodeCount +
        tilePlan.walkthroughNodeCount +
        tilePlan.notRenderingNodeCount;
    if (selectedTraversalCount > 0) {
        return selectedTraversalCount;
    }

    return static_cast<int>(tilePlan.visibleTiles.size());
}

void refreshFrameProgress(TilePlan& tilePlan) {
    tilePlan.frameMappedRasterTileCount = 0;
    tilePlan.frameMappedRasterTileLoadingCount = 0;

    std::unordered_set<TilesetTile*> visitedTiles;
    visitedTiles.reserve(tilePlan.renderEntries.size() * 2);
    auto visitTile = [&](TilesetTile* tile) {
        if (!tile || !visitedTiles.insert(tile).second) {
            return;
        }
        collectMappedRasterProgress(*tile, tilePlan);
    };

    for (TilesetTile* tile : tilePlan.tilesToRenderThisFrame) {
        visitTile(tile);
    }

    for (const TileRenderEntry& entry : tilePlan.renderEntries) {
        visitTile(entry.selectedTile);
        visitTile(entry.renderTile);
    }

    tilePlan.frameProgressTotalCount =
        baseProgressTotalCount(tilePlan) + tilePlan.frameMappedRasterTileCount;
    tilePlan.frameProgressLoadingCount =
        tilePlan.frameMappedRasterTileLoadingCount;
    tilePlan.frameLoadProgressPercentage =
        tilePlan.frameProgressLoadingCount == 0 ||
                tilePlan.frameProgressTotalCount <= 0
            ? 100.0
            : 100.0 *
                  static_cast<double>(
                      tilePlan.frameProgressTotalCount -
                      tilePlan.frameProgressLoadingCount) /
                  static_cast<double>(tilePlan.frameProgressTotalCount);
}

} // namespace

void TileRenderPlanFrameRefresher::refresh(
    TilePlan& tilePlan,
    TileContentAccess& contentAccess,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    const TileRenderPlanFrameRefreshOptions& options) {
    TileRenderPlanFinalizer::refreshRenderEntries(
        tilePlan,
        TileRenderPlanFinalizeOptions{
            options.interactionActive,
            kActiveInteractionRenderPrepBudget,
            kRecoveryRenderPrepBudget,
            options.maximumScreenSpaceError},
        rasterOverlays,
        [&contentAccess](const TileKey& key) {
            return contentAccess.ensureTile(key);
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [&rasterOverlays](const TilesetTile& tile) {
            const bool geometryDrawable =
                tile.content.renderContent.hasDrawableResources();
            return geometryDrawable &&
                   TileRasterOverlayReadinessPolicy::
                       terrainSurfaceImageryDrawableReady(
                           tile,
                           rasterOverlays);
        });
    // 机制 B:渲染集定稿后解析每瓦片 4 边邻居八度差(边吸附输入)。
    TileEdgeSnapResolver::resolve(tilePlan);
    logEdgeMismatch(tilePlan);
    refreshFrameCredits(tilePlan, rasterOverlays);
    refreshFrameProgress(tilePlan);
}

} // namespace earth_engine
