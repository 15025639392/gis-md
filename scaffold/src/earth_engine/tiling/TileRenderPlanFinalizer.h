#pragma once

#include "TileKey.h"
#include "TilePlan.h"
#include "TileRasterOverlayReadinessPolicy.h"
#include "TileSurfaceClip.h"
#include "TilesetTile.h"

#include <algorithm>
#include <array>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;

struct TileRenderPlanFinalizeOptions {
    bool interactionActive = false;
    int activeInteractionRenderPrepBudget = 0;
    int recoveryRenderPrepBudget = 1;
    // Tileset SSE threshold for the distance-continuous geomorph factor
    // (0 = derivation disabled → terrainMorphFactor stays 1 = no morph).
    double maximumScreenSpaceError = 0.0;
    // H-S5:新瓦首建(命令缓存首次构建,~0.9ms/片真机实测)的每帧预算。
    // 预算耗尽且存在可渲染祖先时,该瓦本帧改走祖先裁剪回退、首建顺延
    // (与瞬态面回退同一条已证路径,接缝由机制 A/B 保证);无祖先可盖时
    // 不受限直接建,防露底。扫掠前沿跨瓦边界的集中首建 burst 摊平点。
    int activeInteractionFirstBuildBudget = 4;
    int recoveryFirstBuildBudget = 8;
};

struct TileRenderPlanFinalizer {
    enum class DirectRenderFallbackPolicy {
        PreferAncestorForTransientSurface,
        AllowTransientSurfaceAsLastResort
    };

    /// 「本瓦片此刻能否直接建出渲染条目」的**单一事实源**。除 finalizer 自用,
    /// 遍历器的向下回落守卫(continueDeeper:上帧 Refined 且不可画 → 后代留任)
    /// 也必须用同一份 —— 曾各写一份(遍历只看 mapping 就绪位,这里看绑定级
    /// 纹理可绑+texcoord),暂态分叉时守卫不触发、瓦片被选中却建不出条目,
    /// 外拉漏底(真机 HoleQual notex dropz=1-5 实测)。
    static bool canBuildRenderEntryDirectly(
        const TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        DirectRenderFallbackPolicy fallbackPolicy) {
        // Real terrain can replace its parent immediately. Transient
        // ellipsoid/fill surfaces are drawable, but they should only become
        // direct entries when no renderable ancestor can keep coverage.
        if (tile.content.renderContent.hasDrawableResources()) {
            if (fallbackPolicy ==
                    DirectRenderFallbackPolicy::
                        PreferAncestorForTransientSurface &&
                tile.content.renderContent
                    .drawsTransientFallbackSurface()) {
                return false;
            }
            return TileRasterOverlayReadinessPolicy::
                terrainSurfaceImageryDrawableReady(tile, rasterOverlays);
        }
        return hasRenderableSurfaceForPlan(tile);
    }

    template <typename EnsureTileFn,
              typename CacheKeyFn,
              typename IsFallbackRenderableFn>
    static void refreshRenderEntries(
        TilePlan& plan,
        const TileRenderPlanFinalizeOptions& options,
        EnsureTileFn&& ensureTile,
        CacheKeyFn&& cacheKey,
        IsFallbackRenderableFn&& isFallbackRenderable) {
        static const std::vector<ActivatedRasterOverlay*> kNoRasterOverlays;
        refreshRenderEntries(
            plan,
            options,
            kNoRasterOverlays,
            std::forward<EnsureTileFn>(ensureTile),
            std::forward<CacheKeyFn>(cacheKey),
            std::forward<IsFallbackRenderableFn>(isFallbackRenderable));
    }

    template <typename EnsureTileFn,
              typename CacheKeyFn,
              typename IsFallbackRenderableFn>
    static void refreshRenderEntries(
        TilePlan& plan,
        const TileRenderPlanFinalizeOptions& options,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        EnsureTileFn&& ensureTile,
        CacheKeyFn&& cacheKey,
        IsFallbackRenderableFn&& isFallbackRenderable) {
        (void)cacheKey;
        std::vector<TilesetTile*> selectedTileHandles =
            std::move(plan.tilesToRenderThisFrame);
        plan.renderEntries.clear();
        plan.tilesToRenderThisFrame.clear();
        plan.renderEntryAncestorFallbackCount = 0;
        plan.renderEntrySynchronousPrepCount = 0;
        plan.renderEntryDeferredPrepCount = 0;
        plan.renderEntryFirstBuildDeferredCount = 0;
        plan.renderEntryDropClipUvCount = 0;
        plan.renderEntryDropNotBuildableCount = 0;
        plan.renderEntryBaseColorFallbackCount = 0;
        plan.renderEntryDropNoGeometryCount = 0;
        plan.renderEntryDropNoMappingCount = 0;
        plan.renderEntryDropNoReadyTextureCount = 0;
        plan.renderEntryDropTexcoordInvalidCount = 0;
        plan.renderEntryDropOtherCount = 0;
        plan.renderEntryDropMinZoom = 0;
        plan.renderEntryDropMaxZoom = 0;
        plan.renderEntryDropNoTexZoom = -1;
        plan.renderEntryDropNoTexLoadingState = -1;
        plan.renderEntryDropNoTexReadyState = -1;
        plan.renderEntryDropNoTexReadyHasTexture = 0;
        plan.renderEntryDropNoTexAncestorDepth = 0;
        plan.renderEntryDropNoTexAncestorsWithMapping = 0;
        plan.renderEntryDropNoTexAncestorsWithTexture = 0;
        plan.renderEntryDropNoTexMappingState = -1;
        plan.renderEntryDropNoTexAuthoritativeUpdates = 0;
        plan.renderEntryDropNoTexTileLoadState = -99;
        plan.renderEntryDropNoTexTileContentKind = -1;

        std::unordered_set<RenderGeometryIdentity, RenderGeometryIdentityHash>
            renderedGeometry;
        std::unordered_set<TilesetTile*> renderedFullGeometry;
        std::unordered_set<TilesetTile*> renderedClippedGeometry;

        int renderPrepBudgetRemaining = options.interactionActive
            ? options.activeInteractionRenderPrepBudget
            : options.recoveryRenderPrepBudget;
        int firstBuildBudgetRemaining = options.interactionActive
            ? options.activeInteractionFirstBuildBudget
            : options.recoveryFirstBuildBudget;
        std::unordered_set<TilesetTile*> firstBuildBudgeted;
        auto needsFirstCommandBuild = [](TilesetTile& tile) {
            return !tile.content.renderContent.hasCachedDrawCommands();
        };

        auto appendRenderEntry = [&](TilesetTile& selectedTile) {
            TilesetTile* commandTile = &selectedTile;
            std::optional<std::array<float, 4>> surfaceClipUv;
            bool usesAncestorFallback = false;

            if (!canBuildRenderEntryDirectly(
                    selectedTile,
                    rasterOverlays,
                    DirectRenderFallbackPolicy::
                        PreferAncestorForTransientSurface)) {
                TilesetTile* renderableAncestor =
                    findNearestRenderableAncestor(
                        selectedTile,
                        isFallbackRenderable);
                if (renderableAncestor) {
                    commandTile = renderableAncestor;
                    surfaceClipUv = TileSurfaceClip::forDescendantBounds(
                        *commandTile,
                        selectedTile.bounds);
                    if (!surfaceClipUv) {
                        // 破洞诊断:祖先在手却算不出 clip UV → 该选中瓦片这一帧
                        // 没有任何几何(与下面的 dedup return 不同,那些是"已被
                        // 别的 entry 覆盖",不是洞)。
                        ++plan.renderEntryDropClipUvCount;
                        return;
                    }
                    usesAncestorFallback = true;
                }
            }
            // H-S5:新瓦首建预算。内容已就绪但命令缓存尚未构建的选中瓦片,
            // 预算耗尽时本帧改由最近可渲染祖先裁剪覆盖,首建顺延到后续帧。
            // 预算只摊时间、不丢瓦片;无祖先可盖时保持直建(防露底)。
            if (!usesAncestorFallback && needsFirstCommandBuild(selectedTile) &&
                firstBuildBudgetRemaining <= 0) {
                TilesetTile* renderableAncestor =
                    findNearestRenderableAncestor(
                        selectedTile,
                        isFallbackRenderable);
                if (renderableAncestor) {
                    std::optional<std::array<float, 4>> ancestorClip =
                        TileSurfaceClip::forDescendantBounds(
                            *renderableAncestor,
                            selectedTile.bounds);
                    if (ancestorClip) {
                        commandTile = renderableAncestor;
                        surfaceClipUv = ancestorClip;
                        usesAncestorFallback = true;
                        ++plan.renderEntryFirstBuildDeferredCount;
                    }
                }
            }
            if (!canBuildRenderEntryDirectly(
                    *commandTile,
                    rasterOverlays,
                    DirectRenderFallbackPolicy::
                        AllowTransientSurfaceAsLastResort)) {
                if (!commandTile->content.renderContent
                         .hasDrawableResources()) {
                    // 真无几何(nogeo):无面可画,才丢弃。这是唯一还允许 drop
                    // 的成因,按桶记录用于诊断。
                    ++plan.renderEntryDropNotBuildableCount;
                    recordDropReason(plan, *commandTile, rasterOverlays);
                    return;
                }
                // 几何可画、只差 base 影像(自身与祖先都没有 ready 纹理)——
                // never-drop(cesium 语义):发 entry,命令端 GltfDrawCommandBuilder
                // 出 count=0 的 base 色面(TileRenderCommandPreparer 已把
                // renderability 标 drawable-but-incomplete,细化继续追真影像)。
                // 快拉到影像未下载完的经度时画灰不画黑,绝不留透底洞。
                ++plan.renderEntryBaseColorFallbackCount;
            }

            const RenderGeometryIdentity renderIdentity{
                commandTile,
                surfaceClipUv ? &selectedTile : nullptr};
            if (surfaceClipUv) {
                if (renderedFullGeometry.count(commandTile) > 0) {
                    return;
                }
            } else if (
                renderedClippedGeometry.count(commandTile) > 0) {
                eraseClippedEntriesForRenderTile(
                    plan,
                    commandTile,
                    renderedGeometry,
                    renderedClippedGeometry);
            }
            if (!renderedGeometry.insert(renderIdentity).second) {
                return;
            }
            if (surfaceClipUv) {
                renderedClippedGeometry.insert(commandTile);
            } else {
                renderedFullGeometry.insert(commandTile);
            }

            // H-S5:首建预算消耗(按 commandTile 去重:同一瓦片多 clip entry
            // 只建一次;预算 0 且无祖先的 root 情形已在上面放行直建)。
            if (!usesAncestorFallback &&
                needsFirstCommandBuild(*commandTile) &&
                firstBuildBudgeted.insert(commandTile).second &&
                firstBuildBudgetRemaining > 0) {
                --firstBuildBudgetRemaining;
            }

            bool allowSynchronousMeshPrep = true;
            if (needsSurfaceGeometryPrep(*commandTile)) {
                if (renderPrepBudgetRemaining > 0) {
                    --renderPrepBudgetRemaining;
                    ++plan.renderEntrySynchronousPrepCount;
                } else if (usesAncestorFallback) {
                    allowSynchronousMeshPrep = false;
                    ++plan.renderEntryDeferredPrepCount;
                } else {
                    // Root/no-ancestor case: allow one direct prep to avoid a
                    // blank frame. This path is rare and still bounded by
                    // traversal size.
                    ++plan.renderEntrySynchronousPrepCount;
                }
            }

            TileRenderEntry entry;
            entry.selectedKey = selectedTile.key;
            entry.renderKey = commandTile->key;
            entry.selectedTile = &selectedTile;
            entry.renderTile = commandTile;
            entry.reason = TileRenderEntryReason::Direct;
            entry.usesAncestorFallback = usesAncestorFallback;
            entry.allowSynchronousMeshPrep = allowSynchronousMeshPrep;
            if (surfaceClipUv) {
                entry.surfaceClipEnabled = true;
                entry.surfaceClipUv = *surfaceClipUv;
            }
            if (usesAncestorFallback) {
                entry.reason = TileRenderEntryReason::AncestorFallback;
                ++plan.renderEntryAncestorFallbackCount;
            } else if (
                needsSurfaceGeometryPrep(*commandTile) &&
                allowSynchronousMeshPrep) {
                entry.reason = TileRenderEntryReason::SynchronousPrep;
            }
            plan.renderEntries.push_back(std::move(entry));
        };

        for (size_t i = 0; i < plan.visibleTiles.size(); ++i) {
            const TileKey& key = plan.visibleTiles[i];
            TilesetTile* tile =
                i < selectedTileHandles.size() &&
                        selectedTileHandles[i] &&
                        selectedTileHandles[i]->key == key
                    ? selectedTileHandles[i]
                    : ensureTile(key);
            if (!tile) {
                continue;
            }
            plan.tilesToRenderThisFrame.push_back(tile);
            // 距离连续 geomorph:地形瓦片的 morph 进度由其自身 SSE 在有效 LOD 频带
            // 内的位置决定(而非定时器),随相机连续移动平滑推进,消除硬 pop。halving
            // 四叉树中,父级在 sse=maxSSE/2 时接管(parent.sse≈2·sse≤maxSSE),本瓦片
            // 在 sse=maxSSE 时下钻,故有效频带 (maxSSE/2, maxSSE]:
            //   t = clamp((sse/maxSSE − ½)/½, 0, 1) = clamp(2·sse/maxSSE − 1, 0, 1)
            //   sse→maxSSE(相机近/将下钻)→ 1 全细节;
            //   sse→maxSSE/2(刚从父级细化出)→ 0 粗起点≈父面。
            // morph 纯由 SSE 驱动,故仅 gate 在 maxSSE>0。非规则栅格内容 heightDelta=0 自动无位移(自门控),
            // 非地形不读此值(见 applyPerFrameCommandState)。skirt 遮盖相邻瓦片不同
            // morph 进度间的边缝。
            constexpr double kMorphStartRatio = 0.5;  // parent-takeover 点(halving)
            float terrainMorph = 1.0f;
            if (options.maximumScreenSpaceError > 0.0) {
                const double ratio =
                    tile->selectionFrameState.screenSpaceError /
                    options.maximumScreenSpaceError;
                terrainMorph = static_cast<float>(std::clamp(
                    (ratio - kMorphStartRatio) / (1.0 - kMorphStartRatio),
                    0.0,
                    1.0));
            }
            tile->selectionFrameState.terrainMorphFactor = terrainMorph;
            appendRenderEntry(*tile);
        }
    }

private:
    struct RenderGeometryIdentity {
        TilesetTile* renderTile = nullptr;
        TilesetTile* selectedTileForClip = nullptr;

        bool operator==(const RenderGeometryIdentity& other) const {
            return renderTile == other.renderTile &&
                   selectedTileForClip ==
                       other.selectedTileForClip;
        }
    };

    struct RenderGeometryIdentityHash {
        size_t operator()(const RenderGeometryIdentity& identity) const {
            const size_t renderHash =
                std::hash<TilesetTile*>()(identity.renderTile);
            const size_t selectedHash =
                std::hash<TilesetTile*>()(
                    identity.selectedTileForClip);
            return renderHash ^
                   (selectedHash + 0x9e3779b9 +
                    (renderHash << 6) + (renderHash >> 2));
        }
    };

    // 记录一次 NotBuildable 丢弃的成因,并记下被丢瓦片的 zoom 跨度(判断是粗
    // 层还是叶子层在漏)。
    static void recordDropReason(
        TilePlan& plan,
        const TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
        if (plan.renderEntryDropNotBuildableCount == 1) {
            plan.renderEntryDropMinZoom = tile.key.z;
            plan.renderEntryDropMaxZoom = tile.key.z;
        } else {
            plan.renderEntryDropMinZoom =
                std::min(plan.renderEntryDropMinZoom, tile.key.z);
            plan.renderEntryDropMaxZoom =
                std::max(plan.renderEntryDropMaxZoom, tile.key.z);
        }

        // 几何本身就不可画 —— 与影像无关,补影像兜底救不了这一类。
        if (!tile.content.renderContent.hasDrawableResources() &&
            !hasRenderableSurfaceForPlan(tile)) {
            ++plan.renderEntryDropNoGeometryCount;
            return;
        }
        switch (TileRasterOverlayReadinessPolicy::baseImageryBlockReason(
                    tile,
                    rasterOverlays)) {
            case BaseImageryBlockReason::NoMapping:
                ++plan.renderEntryDropNoMappingCount;
                break;
            case BaseImageryBlockReason::NoReadyTexture:
                ++plan.renderEntryDropNoReadyTextureCount;
                if (plan.renderEntryDropNoTexZoom < 0) {
                    recordNoTextureProbe(plan, tile, rasterOverlays);
                }
                break;
            case BaseImageryBlockReason::TexcoordInvalid:
                ++plan.renderEntryDropTexcoordInvalidCount;
                break;
            case BaseImageryBlockReason::None:
                // 影像可画、几何也在,却仍被拦下 —— 只可能是 transient 代理面
                // 在 PreferAncestor 模式下被让位。计入 other 便于发现漏判。
                ++plan.renderEntryDropOtherCount;
                break;
        }
    }

    // 只记本帧第一片:同帧的 notex 丢弃基本是同一批兄弟,画像相同。
    static void recordNoTextureProbe(
        TilePlan& plan,
        const TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
        const BaseImageryNoTextureProbe probe =
            TileRasterOverlayReadinessPolicy::probeNoReadyTexture(
                tile,
                rasterOverlays);
        if (!probe.valid) {
            return;
        }
        plan.renderEntryDropNoTexZoom = probe.zoom;
        plan.renderEntryDropNoTexLoadingState = probe.loadingState;
        plan.renderEntryDropNoTexReadyState = probe.readyState;
        plan.renderEntryDropNoTexReadyHasTexture =
            probe.readyHasTexture ? 1 : 0;
        plan.renderEntryDropNoTexAncestorDepth = probe.ancestorDepth;
        plan.renderEntryDropNoTexAncestorsWithMapping =
            probe.ancestorsWithMapping;
        plan.renderEntryDropNoTexAncestorsWithTexture =
            probe.ancestorsWithTexture;
        plan.renderEntryDropNoTexMappingState = probe.mappingState;
        plan.renderEntryDropNoTexAuthoritativeUpdates =
            probe.authoritativeUpdates;
        plan.renderEntryDropNoTexTileLoadState = probe.tileLoadState;
        plan.renderEntryDropNoTexTileContentKind = probe.tileContentKind;
    }

    static bool needsSurfaceGeometryPrep(const TilesetTile& tile) {
        static_cast<void>(tile);
        return false;
    }

    static bool hasRenderableSurfaceForPlan(const TilesetTile& tile) {
        return tile.hasSurfaceDrawable();
    }

    template <typename IsFallbackRenderableFn>
    static TilesetTile* findNearestRenderableAncestor(
        TilesetTile& tile,
        IsFallbackRenderableFn&& isFallbackRenderable) {
        for (TilesetTile* ancestor = tile.parent;
             ancestor;
             ancestor = ancestor->parent) {
            if (isFallbackRenderable(*ancestor)) {
                return ancestor;
            }
        }
        return nullptr;
    }

    static void eraseClippedEntriesForRenderTile(
        TilePlan& plan,
        TilesetTile* renderTile,
        std::unordered_set<
            RenderGeometryIdentity,
            RenderGeometryIdentityHash>& renderedGeometry,
        std::unordered_set<TilesetTile*>& renderedClippedGeometry) {
        plan.renderEntries.erase(
            std::remove_if(
                plan.renderEntries.begin(),
                plan.renderEntries.end(),
                [&](const TileRenderEntry& entry) {
                    if (!entry.hasSurfaceClip() ||
                        entry.renderTile != renderTile) {
                        return false;
                    }
                    renderedGeometry.erase(RenderGeometryIdentity{
                        renderTile,
                        entry.selectedTile});
                    if (entry.usesAncestorFallback &&
                        plan.renderEntryAncestorFallbackCount > 0) {
                        --plan.renderEntryAncestorFallbackCount;
                    }
                    return true;
                }),
            plan.renderEntries.end());
        renderedClippedGeometry.erase(renderTile);
    }
};

} // namespace earth_engine
