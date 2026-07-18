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
    bool enableLodTransitionPeriod = false;
    bool interactionActive = false;
    int activeInteractionRenderPrepBudget = 0;
    int recoveryRenderPrepBudget = 1;
    // Tileset SSE threshold for the distance-continuous geomorph factor
    // (0 = derivation disabled → terrainMorphFactor stays 1 = no morph).
    double maximumScreenSpaceError = 0.0;
};

struct TileRenderPlanFinalizer {
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
        std::vector<TilesetTile*> fadingTileHandles =
            std::move(plan.tilesFadingOutThisFrame);
        plan.renderEntries.clear();
        plan.tilesToRenderThisFrame.clear();
        plan.tilesFadingOutThisFrame.clear();
        plan.renderEntryAncestorFallbackCount = 0;
        plan.renderEntrySynchronousPrepCount = 0;
        plan.renderEntryDeferredPrepCount = 0;

        std::unordered_set<RenderGeometryIdentity, RenderGeometryIdentityHash>
            renderedGeometry;
        std::unordered_set<TilesetTile*> renderedFullGeometry;
        std::unordered_set<TilesetTile*> renderedClippedGeometry;

        int renderPrepBudgetRemaining = options.interactionActive
            ? options.activeInteractionRenderPrepBudget
            : options.recoveryRenderPrepBudget;

        auto appendRenderEntry = [&](TilesetTile& selectedTile,
                                     float opacity,
                                     bool selectedThisFrame) {
            TilesetTile* commandTile = &selectedTile;
            std::optional<std::array<float, 4>> surfaceClipUv;
            bool usesAncestorFallback = false;

            if (selectedThisFrame &&
                !canBuildRenderEntryDirectly(
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
                        return;
                    }
                    usesAncestorFallback = true;
                }
            }
            if (!canBuildRenderEntryDirectly(
                    *commandTile,
                    rasterOverlays,
                    DirectRenderFallbackPolicy::
                        AllowTransientSurfaceAsLastResort)) {
                return;
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
            entry.reason = selectedThisFrame
                ? TileRenderEntryReason::Direct
                : TileRenderEntryReason::FadingOut;
            entry.opacity = commandTile == &selectedTile ? opacity : 1.0f;
            entry.selectedThisFrame = selectedThisFrame;
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

        // geomorph 取代 cross-fade(enableLodTransitionPeriod 开启时):地形改用
        // 几何 morph 单层不透明过渡(incoming 子瓦片从平滑起点≈父形状 morph 到真实
        // 高度),**不再渲染 fadingOut 父瓦片基底**。原因:incoming 是不透明(非 alpha
        // 淡入),若同时画不透明的 fadingOut 父瓦片,子瓦片 morph 顶起来下探到父面
        // 下方时会被父深度挡住/穿插→z-fighting 碎裂;而 GltfPrimitive 固定状态
        // 不变量要求 depthTest=true(不能靠关深度绕过)。incoming 从 morph=0 起就
        // 不透明覆盖同区域,父瓦片纯多余。代价:无 incoming 替代的瓦片离场(如地平线
        // 滑出)退化为轻微 pop(可接受)。cross-fade fadingOut 基底仅 geomorph 关时才需。
        if (!options.enableLodTransitionPeriod) {
            for (size_t i = 0; i < plan.tilesFadingOut.size(); ++i) {
                const TileTransition& transition = plan.tilesFadingOut[i];
                if (transition.opacity <= 0.001f) {
                    continue;
                }
                TilesetTile* tile =
                    i < fadingTileHandles.size() &&
                            fadingTileHandles[i] &&
                            fadingTileHandles[i]->key == transition.key
                        ? fadingTileHandles[i]
                        : ensureTile(transition.key);
                if (!tile) {
                    continue;
                }
                plan.tilesFadingOutThisFrame.push_back(tile);
                appendRenderEntry(*tile, transition.opacity, false);
            }
        }

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
            const float transitionOpacity =
                options.enableLodTransitionPeriod
                    ? tile->selectionFrameState.lodTransitionFadePercentage
                    : 1.0f;
            // 距离连续 geomorph:地形瓦片的 morph 进度由其自身 SSE 在有效 LOD 频带
            // 内的位置决定(而非定时器),随相机连续移动平滑推进,消除硬 pop。halving
            // 四叉树中,父级在 sse=maxSSE/2 时接管(parent.sse≈2·sse≤maxSSE),本瓦片
            // 在 sse=maxSSE 时下钻,故有效频带 (maxSSE/2, maxSSE]:
            //   t = clamp((sse/maxSSE − ½)/½, 0, 1) = clamp(2·sse/maxSSE − 1, 0, 1)
            //   sse→maxSSE(相机近/将下钻)→ 1 全细节;
            //   sse→maxSSE/2(刚从父级细化出)→ 0 粗起点≈父面。
            // **与时序 fade(enableLodTransitionPeriod)解耦**:morph 纯由 SSE 驱动、
            // 无需每帧 fade discovery/kick-keeps-fading(那会使运动期工作集膨胀=卡顿),
            // 故仅 gate 在 maxSSE>0。非规则栅格内容 heightDelta=0 自动无位移(自门控),
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
            appendRenderEntry(*tile, transitionOpacity, true);
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

    enum class DirectRenderFallbackPolicy {
        PreferAncestorForTransientSurface,
        AllowTransientSurfaceAsLastResort
    };

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
