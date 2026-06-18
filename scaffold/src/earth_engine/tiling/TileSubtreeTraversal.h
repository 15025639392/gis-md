#pragma once

#include "TileKey.h"
#include "TilesetTile.h"

#include <string>
#include <vector>

namespace earth_engine {

struct TileSubtreeRemovalEntry {
    TilesetTile* tile = nullptr;
    std::string cacheKey;
};

struct TileSubtreeTraversal {
    static std::vector<const TilesetTile*> collectTiles(
        const TilesetTile& root) {
        std::vector<const TilesetTile*> result;
        result.push_back(&root);

        std::vector<const TilesetTile*> stack;
        stack.reserve(root.children.size());
        for (const TilesetTile* child : root.children) {
            if (child) {
                stack.push_back(child);
            }
        }

        while (!stack.empty()) {
            const TilesetTile* current = stack.back();
            stack.pop_back();
            result.push_back(current);
            for (const TilesetTile* child : current->children) {
                if (child) {
                    stack.push_back(child);
                }
            }
        }

        return result;
    }

    template <typename CacheKeyForTileFn>
    static std::vector<std::string> collectCacheKeys(
        const TilesetTile& root,
        CacheKeyForTileFn&& cacheKeyForTile) {
        std::vector<std::string> keys;
        for (const TilesetTile* tile : collectTiles(root)) {
            if (tile) {
                keys.push_back(cacheKeyForTile(tile->key));
            }
        }
        return keys;
    }

    template <typename CacheKeyForTileFn>
    static std::vector<TileSubtreeRemovalEntry> collectDescendantsForRemoval(
        TilesetTile& root,
        CacheKeyForTileFn&& cacheKeyForTile) {
        std::vector<TileSubtreeRemovalEntry> result;

        std::vector<TilesetTile*> stack;
        stack.reserve(root.children.size());
        for (TilesetTile* child : root.children) {
            if (child) {
                stack.push_back(child);
            }
        }

        while (!stack.empty()) {
            TilesetTile* current = stack.back();
            stack.pop_back();
            for (TilesetTile* child : current->children) {
                if (child) {
                    stack.push_back(child);
                }
            }
            result.push_back(TileSubtreeRemovalEntry{
                current,
                cacheKeyForTile(current->key)});
        }

        return result;
    }
};

} // namespace earth_engine
