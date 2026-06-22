#pragma once

#include "TileKey.h"
#include "TilePlan.h"
#include "TilesetTile.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace earth_engine {

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
        plan.renderEntries.clear();
        plan.renderEntryAncestorFallbackCount = 0;
        plan.renderEntrySynchronousPrepCount = 0;
        plan.renderEntryDeferredPrepCount = 0;

        std::unordered_set<std::string> renderedGeometryKeys;
        std::unordered_set<std::string> renderedFullGeometryKeys;
        std::unordered_set<std::string> renderedClippedGeometryKeys;

        int renderPrepBudgetRemaining = options.interactionActive
            ? options.activeInteractionRenderPrepBudget
            : options.recoveryRenderPrepBudget;

        auto appendRenderEntry = [&](const TileKey& key,
                                     float opacity,
                                     bool selectedThisFrame) {
            TilesetTile* selectedTile = ensureTile(key);
            if (!selectedTile) return;

            TilesetTile* commandTile = selectedTile;
            std::optional<std::array<float, 4>> surfaceClipUv;
            bool usesAncestorFallback = false;

            if (selectedThisFrame &&
                !canBuildRenderEntryDirectly(*selectedTile)) {
                TilesetTile* renderableAncestor =
                    findNearestRenderableAncestor(
                        *selectedTile,
                        isFallbackRenderable);
                if (renderableAncestor) {
                    commandTile = renderableAncestor;
                    surfaceClipUv = clipUvForDescendantBounds(
                        commandTile->bounds,
                        selectedTile->bounds);
                    if (!surfaceClipUv) {
                        return;
                    }
                    usesAncestorFallback = true;
                }
            }

            const std::string selectedCk = cacheKey(selectedTile->key);
            const std::string commandCk = cacheKey(commandTile->key);
            std::string renderDedupKey = commandCk;
            if (surfaceClipUv) {
                if (renderedFullGeometryKeys.count(commandCk) > 0) {
                    return;
                }
                renderDedupKey += "|clip:";
                renderDedupKey += selectedCk;
            } else if (renderedClippedGeometryKeys.count(commandCk) > 0) {
                eraseClippedEntriesForRenderTile(
                    plan,
                    commandCk,
                    renderedGeometryKeys,
                    renderedClippedGeometryKeys,
                    cacheKey);
            }
            if (!renderedGeometryKeys.insert(renderDedupKey).second) {
                return;
            }
            if (surfaceClipUv) {
                renderedClippedGeometryKeys.insert(commandCk);
            } else {
                renderedFullGeometryKeys.insert(commandCk);
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
            entry.selectedKey = selectedTile->key;
            entry.renderKey = commandTile->key;
            entry.reason = selectedThisFrame
                ? TileRenderEntryReason::Direct
                : TileRenderEntryReason::FadingOut;
            entry.opacity = commandTile == selectedTile ? opacity : 1.0f;
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

        for (const TileKey& key : plan.visibleTiles) {
            TilesetTile* tile = ensureTile(key);
            const float transitionOpacity =
                options.enableLodTransitionPeriod && tile
                    ? tile->selectionFrameState.lodTransitionFadePercentage
                    : 1.0f;
            appendRenderEntry(key, transitionOpacity, true);
        }

        for (const TileTransition& transition : plan.tilesFadingOut) {
            if (transition.opacity <= 0.001f) {
                continue;
            }
            appendRenderEntry(transition.key, transition.opacity, false);
        }
    }

private:
    static bool canBuildRenderEntryDirectly(const TilesetTile& tile) {
        return tile.content.renderContent.hasGltfContent() ||
               tile.hasSurfaceDrawable();
    }

    static bool needsSurfaceGeometryPrep(const TilesetTile& tile) {
        return !tile.content.renderContent.hasGltfContent() &&
               !tile.hasSurfaceDrawable();
    }

    static std::optional<std::array<float, 4>> clipUvForDescendantBounds(
        const Rectangle& ancestorBounds,
        const Rectangle& descendantBounds) {
        constexpr double kTwoPiForLongitudeWrap =
            3.14159265358979323846264338327950288 * 2.0;
        const double ancestorWidth = ancestorBounds.width();
        const double ancestorHeight = ancestorBounds.height();
        if (ancestorWidth <= 0.0 || ancestorHeight <= 0.0) {
            return std::nullopt;
        }

        auto longitudeOffset = [&](double longitude) {
            double offset = longitude - ancestorBounds.west();
            if (ancestorBounds.crossesAntimeridian() && offset < 0.0) {
                offset += kTwoPiForLongitudeWrap;
            }
            return offset;
        };

        double uMin = longitudeOffset(descendantBounds.west()) / ancestorWidth;
        double uMax = longitudeOffset(descendantBounds.east()) / ancestorWidth;
        if (ancestorBounds.crossesAntimeridian() && uMax < uMin) {
            uMax += 1.0;
        }
        double vMin = (ancestorBounds.north() - descendantBounds.north()) /
                      ancestorHeight;
        double vMax = (ancestorBounds.north() - descendantBounds.south()) /
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

    template <typename CacheKeyFn>
    static void eraseClippedEntriesForRenderTile(
        TilePlan& plan,
        const std::string& renderCacheKey,
        std::unordered_set<std::string>& renderedGeometryKeys,
        std::unordered_set<std::string>& renderedClippedGeometryKeys,
        CacheKeyFn&& cacheKey) {
        plan.renderEntries.erase(
            std::remove_if(
                plan.renderEntries.begin(),
                plan.renderEntries.end(),
                [&](const TileRenderEntry& entry) {
                    if (!entry.hasSurfaceClip() ||
                        cacheKey(entry.renderKey) != renderCacheKey) {
                        return false;
                    }
                    std::string clippedDedupKey = renderCacheKey;
                    clippedDedupKey += "|clip:";
                    clippedDedupKey += cacheKey(entry.selectedKey);
                    renderedGeometryKeys.erase(clippedDedupKey);
                    if (entry.usesAncestorFallback &&
                        plan.renderEntryAncestorFallbackCount > 0) {
                        --plan.renderEntryAncestorFallbackCount;
                    }
                    return true;
                }),
            plan.renderEntries.end());
        renderedClippedGeometryKeys.erase(renderCacheKey);
    }
};

} // namespace earth_engine
