#include "SceneTilesetCoordinator.h"

#include "FrameState.h"
#include "ScenePrimaryTilesetTakeoverPolicy.h"
#include "../debug/PerfTimer.h"
#include "../debug/PlatformLog.h"
#include "../tiling/Tileset.h"

#include <utility>

namespace earth_engine {

SceneTilesetCoordinator::~SceneTilesetCoordinator() = default;

void SceneTilesetCoordinator::setPrimary(std::unique_ptr<Tileset> tileset) {
    pendingPrimary_.reset();
    pendingTakeoverState_ = {};
    primary_ = std::move(tileset);
    if (primary_) {
        applyOcclusionCallback(*primary_);
    }
}

void SceneTilesetCoordinator::stagePrimaryReplacement(
    std::unique_ptr<Tileset> tileset) {
    pendingPrimary_ = std::move(tileset);
    pendingTakeoverState_ = {};
    if (pendingPrimary_) {
        applyOcclusionCallback(*pendingPrimary_);
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
    if (pendingPrimary_) {
        applyOcclusionCallback(*pendingPrimary_);
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
    if (pendingPrimary_) {
        pendingPrimary_->clearOcclusionCallback();
    }
    for (auto& tileset : contentTilesets_) {
        if (tileset) {
            tileset->clearOcclusionCallback();
        }
    }
}

SceneTilesetUpdateResult SceneTilesetCoordinator::update(
    FrameState& frameState,
    IPrepareRendererResources* pPrepRenderer) {
    SceneTilesetUpdateResult result;
    if (primary_) {
        const double startMs = perf::nowMs();
        primary_->update(frameState, pPrepRenderer);
        result.terrainUpdateMs = perf::nowMs() - startMs;
    }
    if (pendingPrimary_) {
        const double startMs = perf::nowMs();
        pendingPrimary_->update(frameState, pPrepRenderer);
        result.terrainUpdateMs += perf::nowMs() - startMs;
        if (!primary_ ||
            ScenePrimaryTilesetTakeoverPolicy::shouldCommit(
                pendingTakeoverState_,
                *primary_,
                *pendingPrimary_)) {
            primary_ = std::move(pendingPrimary_);
            pendingTakeoverState_ = {};
            platformLog(
                LogLevel::Info,
                "Tileset",
                "Staged primary terrain takeover committed");
        }
    }
    if (!contentTilesets_.empty()) {
        const double startMs = perf::nowMs();
        int idx = 0;
        for (auto& tileset : contentTilesets_) {
            if (tileset) {
                const double t0 = perf::nowMs();
                tileset->update(frameState, pPrepRenderer);
                const double t_tile = perf::nowMs() - t0;
                if (t_tile > 5.0) {
                    platformLog(LogLevel::Info, "EarthPerf",
                        "ContentTileset[%d].update: %.2f ms", idx, t_tile);
                }
            }
            ++idx;
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
