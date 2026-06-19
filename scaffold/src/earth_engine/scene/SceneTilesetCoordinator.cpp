#include "SceneTilesetCoordinator.h"

#include "FrameState.h"
#include "../debug/PerfTimer.h"
#include "../tiling/Tileset.h"

#include <utility>

namespace earth_engine {

SceneTilesetCoordinator::~SceneTilesetCoordinator() = default;

void SceneTilesetCoordinator::setPrimary(std::unique_ptr<Tileset> tileset) {
    primary_ = std::move(tileset);
    if (primary_) {
        applyOcclusionCallback(*primary_);
    }
}

void SceneTilesetCoordinator::addContent(std::unique_ptr<Tileset> tileset) {
    if (!tileset) {
        return;
    }
    applyOcclusionCallback(*tileset);
    contentTilesets_.push_back(std::move(tileset));
}

void SceneTilesetCoordinator::setOcclusionCallback(
    TileOcclusionCallback callback) {
    occlusionCallback_ = std::move(callback);
    if (primary_) {
        applyOcclusionCallback(*primary_);
    }
    for (auto& tileset : contentTilesets_) {
        if (tileset) {
            applyOcclusionCallback(*tileset);
        }
    }
}

void SceneTilesetCoordinator::clearOcclusionCallback() {
    occlusionCallback_ = nullptr;
    if (primary_) {
        primary_->clearOcclusionCallback();
    }
    for (auto& tileset : contentTilesets_) {
        if (tileset) {
            tileset->clearOcclusionCallback();
        }
    }
}

SceneTilesetUpdateResult SceneTilesetCoordinator::update(
    FrameState& frameState) {
    SceneTilesetUpdateResult result;
    if (primary_) {
        const double startMs = perf::nowMs();
        primary_->update(frameState);
        result.terrainUpdateMs = perf::nowMs() - startMs;
    }
    if (!contentTilesets_.empty()) {
        const double startMs = perf::nowMs();
        for (auto& tileset : contentTilesets_) {
            if (tileset) {
                tileset->update(frameState);
            }
        }
        result.contentTilesetUpdateMs = perf::nowMs() - startMs;
    }
    return result;
}

void SceneTilesetCoordinator::applyOcclusionCallback(Tileset& tileset) const {
    if (occlusionCallback_) {
        tileset.setOcclusionCallback(occlusionCallback_);
    } else {
        tileset.clearOcclusionCallback();
    }
}

} // namespace earth_engine
