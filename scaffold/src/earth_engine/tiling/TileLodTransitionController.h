#pragma once

#include "TileKey.h"
#include "TilePlan.h"
#include "TilesetTile.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace earth_engine {

struct TileLodTransitionOptions {
    // Map used only for O(1) key→tile lookups (fadingKeys / visibleTiles).
    const std::unordered_map<
        std::string,
        std::unique_ptr<TilesetTile>>* tilesByCacheKey = nullptr;
    // Selection active-set: the tiles the fade-out discovery loop scans for
    // "rendered last frame but not this frame". Bounded to O(visible + fading)
    // instead of the whole registry (every rendered-last-frame tile is carried
    // in the active-set by resetActiveSelectionState).
    const std::vector<TilesetTile*>* activeTiles = nullptr;
    bool enableLodTransitionPeriod = false;
    double lodTransitionLength = 1.0;
};

struct TileLodTransitionController {
    template <typename CacheKeyFn, typename HasTransitionRenderContentFn>
    static void updateTransitions(
        TilePlan& plan,
        std::unordered_set<std::string>& fadingKeys,
        double deltaSeconds,
        const TileLodTransitionOptions& options,
        CacheKeyFn&& cacheKey,
        HasTransitionRenderContentFn&& hasTransitionRenderContent) {
        plan.tilesFadingOut.clear();
        plan.tileTransitions.clear();
        plan.fadingNodeCount = 0;

        std::unordered_set<std::string> currentRenderKeys;
        currentRenderKeys.reserve(plan.visibleTiles.size());
        for (const TileKey& key : plan.visibleTiles) {
            currentRenderKeys.insert(cacheKey(key));
        }

        if (!options.enableLodTransitionPeriod) {
            fadingKeys.clear();
            for (const TileKey& key : plan.visibleTiles) {
                if (TilesetTile* tile =
                        findTileByCacheKey(options, cacheKey(key))) {
                    tile->selectionFrameState.lodTransitionFadePercentage =
                        1.0f;
                }
            }
            return;
        }

        const float transitionDelta = static_cast<float>(
            std::max(0.0, deltaSeconds) /
            std::max(1e-6, options.lodTransitionLength));

        if (options.activeTiles) {
            for (TilesetTile* tile : *options.activeTiles) {
                if (!tile) continue;
                const std::string ck = cacheKey(tile->key);
                if (currentRenderKeys.find(ck) != currentRenderKeys.end()) {
                    continue;
                }
                if (!wasRenderedInPreviousSelection(*tile) ||
                    !hasTransitionRenderContent(*tile)) {
                    continue;
                }
                if (fadingKeys.insert(ck).second) {
                    tile->selectionFrameState.lodTransitionFadePercentage =
                        0.0f;
                }
            }
        }

        std::unordered_set<std::string> returnedFromFadeOut;
        for (auto it = fadingKeys.begin(); it != fadingKeys.end();) {
            if (currentRenderKeys.find(*it) != currentRenderKeys.end()) {
                if (TilesetTile* tile = findTileByCacheKey(options, *it)) {
                    tile->selectionFrameState.lodTransitionFadePercentage =
                        0.0f;
                }
                returnedFromFadeOut.insert(*it);
                it = fadingKeys.erase(it);
                continue;
            }

            TilesetTile* tile = findTileByCacheKey(options, *it);
            if (!tile || !hasTransitionRenderContent(*tile)) {
                it = fadingKeys.erase(it);
                continue;
            }

            TileSelectionFrameState& selection = tile->selectionFrameState;
            if (selection.lodTransitionFadePercentage >= 1.0f) {
                selection.lodTransitionFadePercentage = 0.0f;
                it = fadingKeys.erase(it);
                continue;
            }

            selection.lodTransitionFadePercentage = std::min(
                selection.lodTransitionFadePercentage + transitionDelta,
                1.0f);
            const float renderOpacity =
                1.0f - selection.lodTransitionFadePercentage;
            plan.tilesFadingOut.push_back(TileTransition{
                tile->key,
                renderOpacity,
                1});
            plan.tileTransitions.push_back(TileTransition{
                tile->key,
                renderOpacity,
                1});
            if (renderOpacity > 0.001f) {
                ++plan.fadingNodeCount;
            }
            ++it;
        }

        for (const TileKey& key : plan.visibleTiles) {
            const std::string ck = cacheKey(key);
            TilesetTile* tile = findTileByCacheKey(options, ck);
            if (!tile || !hasTransitionRenderContent(*tile)) {
                continue;
            }
            const bool wasFadingOut =
                returnedFromFadeOut.find(ck) != returnedFromFadeOut.end() ||
                fadingKeys.erase(ck) > 0;
            if (wasFadingOut || !wasRenderedInPreviousSelection(*tile)) {
                tile->selectionFrameState.lodTransitionFadePercentage = 0.0f;
            }

            TileSelectionFrameState& selection = tile->selectionFrameState;
            selection.lodTransitionFadePercentage = std::min(
                selection.lodTransitionFadePercentage + transitionDelta,
                1.0f);
            plan.tileTransitions.push_back(TileTransition{
                tile->key,
                selection.lodTransitionFadePercentage,
                1});
            if (selection.lodTransitionFadePercentage < 0.999f) {
                ++plan.fadingNodeCount;
            }
        }
    }

private:
    static TilesetTile* findTileByCacheKey(
        const TileLodTransitionOptions& options,
        const std::string& cacheKey) {
        if (!options.tilesByCacheKey) {
            return nullptr;
        }
        auto it = options.tilesByCacheKey->find(cacheKey);
        return it == options.tilesByCacheKey->end() || !it->second
            ? nullptr
            : it->second.get();
    }

    static bool wasRenderedInPreviousSelection(const TilesetTile& tile) {
        const TileSelectionState previous =
            tile.selectionFrameState.previousSelectionState;
        return previous == TileSelectionState::Rendered ||
               (previous == TileSelectionState::Refined &&
                tile.refine == TileRefine::Add);
    }
};

} // namespace earth_engine
