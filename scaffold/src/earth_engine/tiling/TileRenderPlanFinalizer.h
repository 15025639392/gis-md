#pragma once

#include "TileKey.h"
#include "TilePlan.h"
#include "TileRasterOverlayReadinessPolicy.h"
#include "TilesetTile.h"

#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Projection.h"
#include "../core/geodesy/WebMercatorProjection.h"

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
                    surfaceClipUv = clipUvForDescendantBounds(
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
            appendRenderEntry(*tile, transitionOpacity, true);
        }

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

    static std::optional<std::array<float, 4>> clipUvForDescendantBounds(
        const TilesetTile& commandTile,
        const Rectangle& descendantBounds) {
        // The terrain shader clips against texcoord set 0, which is
        // normalized against the tile content's FIRST overlay-projection
        // rectangle in PROJECTED space with V growing south->north (both QM
        // native UVs and the rewritten web-mercator texcoords keep V=0 at
        // the south edge — see rewriteTerrainProjectionTexCoords). The clip
        // window must be computed in that same space: a linear-geographic,
        // north-down window discards a vertically mirrored region and, on
        // web-mercator groups, drifts with latitude.
        constexpr double kTwoPiForLongitudeWrap =
            3.14159265358979323846264338327950288 * 2.0;

        Rectangle texcoordRect = commandTile.bounds;
        RasterOverlayProjection projection =
            RasterOverlayProjection::Geographic;
        const TileFillGeometrySignature* fillSignature =
            commandTile.content.renderContent.fillGeometrySignature();
        if (commandTile.content.renderContent.drawsFill() &&
            fillSignature) {
            projection = fillSignature->projection;
            texcoordRect =
                projection == RasterOverlayProjection::WebMercator
                ? projectRectangleSimple(
                      WebMercatorProjection(Ellipsoid::WGS84()),
                      fillSignature->bounds
                          .splitAtAntimeridian()
                          .first)
                : fillSignature->bounds;
        } else if (
            commandTile.content.renderContent
                .hasRasterOverlayDetailsContent()) {
            const RasterOverlayDetails& details =
                commandTile.content.renderContent.rasterOverlayDetails();
            if (!details.rasterOverlayProjections.empty() &&
                !details.rasterOverlayRectangles.empty() &&
                !details.rasterOverlayRectangles[0].isEmpty()) {
                projection = details.rasterOverlayProjections[0];
                texcoordRect = details.rasterOverlayRectangles[0];
            }
        }

        const Rectangle descendantProjected =
            projection == RasterOverlayProjection::WebMercator
                ? projectRectangleSimple(
                      WebMercatorProjection(Ellipsoid::WGS84()),
                      descendantBounds.splitAtAntimeridian().first)
                : descendantBounds;

        // Mercator x equals longitude radians, so wrap-aware width works for
        // both projections; projected height needs the plain subtraction.
        const double ancestorWidth = texcoordRect.width();
        const double ancestorHeight = texcoordRect.computeHeight();
        if (ancestorWidth <= 0.0 || ancestorHeight <= 0.0) {
            return std::nullopt;
        }

        auto horizontalOffset = [&](double x) {
            double offset = x - texcoordRect.west();
            if (texcoordRect.crossesAntimeridian() && offset < 0.0) {
                offset += kTwoPiForLongitudeWrap;
            }
            return offset;
        };

        double uMin =
            horizontalOffset(descendantProjected.west()) / ancestorWidth;
        double uMax =
            horizontalOffset(descendantProjected.east()) / ancestorWidth;
        if (texcoordRect.crossesAntimeridian() && uMax < uMin) {
            uMax += 1.0;
        }
        // NW 约定（v=0 在北）：与 overlay texcoord 同基准
        double vMin = (texcoordRect.north() - descendantProjected.north()) /
                      ancestorHeight;
        double vMax = (texcoordRect.north() - descendantProjected.south()) /
                      ancestorHeight;

        uMin = std::clamp(uMin, 0.0, 1.0);
        uMax = std::clamp(uMax, 0.0, 1.0);
        vMin = std::clamp(vMin, 0.0, 1.0);
        vMax = std::clamp(vMax, 0.0, 1.0);
        if (uMax <= uMin || vMax <= vMin) {
            return std::nullopt;
        }

        return std::array<float, 4>{
            static_cast<float>(uMin),
            static_cast<float>(vMin),
            static_cast<float>(uMax - uMin),
            static_cast<float>(vMax - vMin)};
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
