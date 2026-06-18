#pragma once

#include "TileKey.h"
#include "TilePlan.h"
#include "TilesetTile.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace earth_engine {

struct TileLodTransitionOptions {
    const std::unordered_map<
        std::string,
        std::unique_ptr<TilesetTile>>* tilesByCacheKey = nullptr;
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
                    tile->lodTransitionFadePercentage = 1.0f;
                }
            }
            return;
        }

        const float transitionDelta = static_cast<float>(
            std::max(0.0, deltaSeconds) /
            std::max(1e-6, options.lodTransitionLength));

        if (options.tilesByCacheKey) {
            for (const auto& [ck, tile] : *options.tilesByCacheKey) {
                if (!tile) continue;
                if (currentRenderKeys.find(ck) != currentRenderKeys.end()) {
                    continue;
                }
                if (!wasRenderedInPreviousSelection(*tile) ||
                    !hasTransitionRenderContent(*tile)) {
                    continue;
                }
                if (fadingKeys.insert(ck).second) {
                    tile->lodTransitionFadePercentage = 0.0f;
                }
            }
        }

        std::unordered_set<std::string> returnedFromFadeOut;
        for (auto it = fadingKeys.begin(); it != fadingKeys.end();) {
            if (currentRenderKeys.find(*it) != currentRenderKeys.end()) {
                if (TilesetTile* tile = findTileByCacheKey(options, *it)) {
                    tile->lodTransitionFadePercentage = 0.0f;
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

            if (tile->lodTransitionFadePercentage >= 1.0f) {
                tile->lodTransitionFadePercentage = 0.0f;
                it = fadingKeys.erase(it);
                continue;
            }

            tile->lodTransitionFadePercentage = std::min(
                tile->lodTransitionFadePercentage + transitionDelta,
                1.0f);
            const float renderOpacity =
                1.0f - tile->lodTransitionFadePercentage;
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
                tile->lodTransitionFadePercentage = 0.0f;
            }

            tile->lodTransitionFadePercentage = std::min(
                tile->lodTransitionFadePercentage + transitionDelta,
                1.0f);
            plan.tileTransitions.push_back(TileTransition{
                tile->key,
                tile->lodTransitionFadePercentage,
                1});
            if (tile->lodTransitionFadePercentage < 0.999f) {
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
        return tile.previousSelectionState == TileSelectionState::Rendered ||
               (tile.previousSelectionState == TileSelectionState::Refined &&
                tile.refine == TileRefine::Add);
    }
};

} // namespace earth_engine
