#pragma once

#include "TileLoadTypes.h"
#include "../core/resources/FrameResourceBudget.h"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <unordered_set>

namespace earth_engine {

enum class PendingLoadFinalizeKind {
    Terrain,
    Content
};

struct PendingLoadFinalize {
    PendingLoadFinalizeKind kind = PendingLoadFinalizeKind::Terrain;
    std::optional<PendingTerrainUpload> terrainUpload;
    std::optional<PendingContentUpload> contentUpload;
};

struct PendingLoadFinalizeContext {
    bool interactionActive = false;
    FrameResourceBudget& budget;
};

enum class PendingTerminalResultKind {
    Terrain,
    Content
};

struct PendingTerminalResult {
    PendingTerminalResultKind kind = PendingTerminalResultKind::Terrain;
    std::optional<PendingTerrainTerminalResult> terrainResult;
    std::optional<PendingContentTerminalResult> contentResult;
};

class TilePendingLoadQueue {
public:
    bool containsCacheKey(const std::string& cacheKey) const;

    void addTerrainUpload(PendingTerrainUpload upload);
    void addTerrainTerminalResult(PendingTerrainTerminalResult result);
    void addContentUpload(PendingContentUpload upload);
    void addContentTerminalResult(PendingContentTerminalResult result);

    void eraseTerrainUploadKey(const std::string& cacheKey);
    void eraseContentUploadKey(const std::string& cacheKey);
    void eraseCacheKey(const std::string& cacheKey);
    void clear();

    bool hasWork() const;
    size_t terrainUploadCount() const;
    size_t terrainTerminalResultCount() const;
    size_t contentUploadCount() const;
    size_t contentTerminalResultCount() const;

    std::optional<PendingTerrainTerminalResult>
    takeHighestPriorityTerrainTerminalResult();
    std::optional<PendingContentTerminalResult>
    takeHighestPriorityContentTerminalResult();
    std::optional<PendingTerminalResult> takeHighestPriorityTerminalResult(
        FrameResourceBudget& budget);
    std::optional<PendingLoadFinalize> takeHighestPriorityUpload(
        PendingLoadFinalizeContext context);
    std::optional<PendingLoadFinalize> takeHighestPriorityUpload(
        bool interactionActive,
        FrameResourceBudget& budget);

private:
    std::unordered_set<std::string> terrainUploadKeys_;
    std::unordered_set<std::string> contentUploadKeys_;
    std::deque<PendingTerrainUpload> terrainUploads_;
    std::deque<PendingTerrainTerminalResult> terrainTerminalResults_;
    std::deque<PendingContentUpload> contentUploads_;
    std::deque<PendingContentTerminalResult> contentTerminalResults_;
};

} // namespace earth_engine
