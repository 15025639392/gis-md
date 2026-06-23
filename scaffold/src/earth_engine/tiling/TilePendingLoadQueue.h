#pragma once

#include "TileLoadTypes.h"
#include "../core/resources/FrameResourceBudget.h"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <unordered_set>

namespace earth_engine {

struct PendingLoadFinalizeContext {
    bool interactionActive = false;
    FrameResourceBudget& budget;
};

class TilePendingLoadQueue {
public:
    bool containsCacheKey(const std::string& cacheKey) const;

    void addUpload(PendingTileLoad upload);
    void addTerminalResult(PendingTileLoad result);

    void eraseUploadKey(const std::string& cacheKey);
    void eraseCacheKey(const std::string& cacheKey);
    void clear();

    bool hasWork() const;
    size_t uploadCount() const;
    size_t terminalResultCount() const;
    size_t gltfTerrainUploadCount() const;
    size_t gltfTerrainTerminalResultCount() const;
    size_t contentUploadCount() const;
    size_t contentTerminalResultCount() const;

    std::optional<PendingTileLoad> takeHighestPriorityTerminalResult(
        FrameResourceBudget& budget);
    std::optional<PendingTileLoad> takeHighestPriorityUpload(
        PendingLoadFinalizeContext context);
    std::optional<PendingTileLoad> takeHighestPriorityUpload(
        bool interactionActive,
        FrameResourceBudget& budget);

private:
    static size_t countDomain(const std::deque<PendingTileLoad>& loads,
                              TileLoadDomain domain);
    std::unordered_set<std::string> uploadKeys_;
    std::deque<PendingTileLoad> uploads_;
    std::deque<PendingTileLoad> terminalResults_;
};

} // namespace earth_engine
