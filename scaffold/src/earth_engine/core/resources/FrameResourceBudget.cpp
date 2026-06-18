#include "FrameResourceBudget.h"

#include <algorithm>

namespace earth_engine {

void FrameResourceBudget::beginFrame(
    uint64_t frameNumber,
    const FrameResourceBudgetConfig& config) {
    frameNumber_ = frameNumber;
    config_ = config;
    networkRequestsIssued_ = 0;
    terrainContentNetworkRequestsIssued_ = 0;
    rasterNetworkRequestsIssued_ = 0;
    mainThreadFinalizesUsed_ = 0;
    rasterUploadsUsed_ = 0;
    mainThreadElapsedMs_ = 0.0;
}

bool FrameResourceBudget::canIssue(FrameResourceLane lane,
                                   FrameResourcePriority,
                                   int estimatedFanout) const {
    switch (lane) {
        case FrameResourceLane::TerrainRequest:
        case FrameResourceLane::ContentRequest:
            return terrainContentNetworkRequestsIssued_ +
                       positiveUnits(estimatedFanout) <=
                   networkRequestLimit(lane);
        case FrameResourceLane::RasterRequest:
            return rasterNetworkRequestsIssued_ + positiveUnits(estimatedFanout) <=
                   networkRequestLimit(lane);
        case FrameResourceLane::TerrainFinalize:
        case FrameResourceLane::ContentFinalize:
        case FrameResourceLane::RasterTextureUpload:
        case FrameResourceLane::TerminalState:
            return true;
    }
    return true;
}

bool FrameResourceBudget::tryIssue(FrameResourceLane lane,
                                   FrameResourcePriority priority,
                                   int estimatedFanout) {
    if (!canIssue(lane, priority, estimatedFanout)) {
        return false;
    }
    switch (lane) {
        case FrameResourceLane::TerrainRequest:
        case FrameResourceLane::ContentRequest:
            terrainContentNetworkRequestsIssued_ += positiveUnits(estimatedFanout);
            networkRequestsIssued_ += positiveUnits(estimatedFanout);
            break;
        case FrameResourceLane::RasterRequest:
            rasterNetworkRequestsIssued_ += positiveUnits(estimatedFanout);
            networkRequestsIssued_ += positiveUnits(estimatedFanout);
            break;
        case FrameResourceLane::TerrainFinalize:
        case FrameResourceLane::ContentFinalize:
        case FrameResourceLane::RasterTextureUpload:
        case FrameResourceLane::TerminalState:
            break;
    }
    return true;
}

bool FrameResourceBudget::hasNetworkInflightCapacity(
    FrameResourceLane lane,
    uint32_t currentInflight,
    int estimatedFanout) const {
    return currentInflight + positiveUnits(estimatedFanout) <=
           networkInflightLimit(lane);
}

bool FrameResourceBudget::hasNetworkInflightCapacity(
    uint32_t currentInflight,
    int estimatedFanout) const {
    return hasNetworkInflightCapacity(
        FrameResourceLane::TerrainRequest,
        currentInflight,
        estimatedFanout);
}

bool FrameResourceBudget::canFinalize(FrameResourceLane lane,
                                      FrameResourcePriority,
                                      int estimatedCostUnits) const {
    const uint32_t units = positiveUnits(estimatedCostUnits);
    if (mainThreadTimeExpired()) {
        return false;
    }

    switch (lane) {
        case FrameResourceLane::RasterTextureUpload:
            return rasterUploadsUsed_ + units <= config_.maxRasterUploadsPerFrame;
        case FrameResourceLane::TerrainFinalize:
        case FrameResourceLane::ContentFinalize:
            return mainThreadFinalizesUsed_ + units <=
                   config_.maxMainThreadFinalizesPerFrame;
        case FrameResourceLane::TerminalState:
            return true;
        case FrameResourceLane::TerrainRequest:
        case FrameResourceLane::ContentRequest:
        case FrameResourceLane::RasterRequest:
            return canIssue(lane, FrameResourcePriority::Normal, estimatedCostUnits);
    }
    return true;
}

bool FrameResourceBudget::tryFinalize(FrameResourceLane lane,
                                      FrameResourcePriority priority,
                                      int estimatedCostUnits) {
    if (!canFinalize(lane, priority, estimatedCostUnits)) {
        return false;
    }

    const uint32_t units = positiveUnits(estimatedCostUnits);
    switch (lane) {
        case FrameResourceLane::RasterTextureUpload:
            rasterUploadsUsed_ += units;
            break;
        case FrameResourceLane::TerrainFinalize:
        case FrameResourceLane::ContentFinalize:
            mainThreadFinalizesUsed_ += units;
            break;
        case FrameResourceLane::TerminalState:
        case FrameResourceLane::TerrainRequest:
        case FrameResourceLane::ContentRequest:
        case FrameResourceLane::RasterRequest:
            break;
    }
    return true;
}

void FrameResourceBudget::recordElapsed(FrameResourceLane, double elapsedMs) {
    mainThreadElapsedMs_ += std::max(0.0, elapsedMs);
}

bool FrameResourceBudget::mainThreadTimeExpired() const {
    return config_.mainThreadTimeMs > 0.0 &&
           mainThreadElapsedMs_ >= config_.mainThreadTimeMs;
}

uint32_t FrameResourceBudget::positiveUnits(int estimatedUnits) {
    return static_cast<uint32_t>(std::max(1, estimatedUnits));
}

uint32_t FrameResourceBudget::networkRequestLimit(FrameResourceLane lane) const {
    switch (lane) {
        case FrameResourceLane::TerrainRequest:
        case FrameResourceLane::ContentRequest:
            return config_.maxTerrainContentNetworkRequestsPerFrame > 0
                       ? config_.maxTerrainContentNetworkRequestsPerFrame
                       : config_.maxNetworkRequestsPerFrame;
        case FrameResourceLane::RasterRequest:
            return config_.maxRasterNetworkRequestsPerFrame > 0
                       ? config_.maxRasterNetworkRequestsPerFrame
                       : config_.maxNetworkRequestsPerFrame;
        case FrameResourceLane::TerrainFinalize:
        case FrameResourceLane::ContentFinalize:
        case FrameResourceLane::RasterTextureUpload:
        case FrameResourceLane::TerminalState:
            return config_.maxNetworkRequestsPerFrame;
    }
    return config_.maxNetworkRequestsPerFrame;
}

uint32_t FrameResourceBudget::networkInflightLimit(FrameResourceLane lane) const {
    switch (lane) {
        case FrameResourceLane::TerrainRequest:
        case FrameResourceLane::ContentRequest:
            return config_.maxTerrainContentNetworkInflight > 0
                       ? config_.maxTerrainContentNetworkInflight
                       : config_.maxNetworkInflight;
        case FrameResourceLane::RasterRequest:
            return config_.maxRasterNetworkInflight > 0
                       ? config_.maxRasterNetworkInflight
                       : config_.maxNetworkInflight;
        case FrameResourceLane::TerrainFinalize:
        case FrameResourceLane::ContentFinalize:
        case FrameResourceLane::RasterTextureUpload:
        case FrameResourceLane::TerminalState:
            return config_.maxNetworkInflight;
    }
    return config_.maxNetworkInflight;
}

} // namespace earth_engine
