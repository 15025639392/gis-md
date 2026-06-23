#pragma once

#include <cstdint>

namespace earth_engine {

enum class FrameResourceLane {
    TerrainRequest,
    ContentRequest,
    RasterRequest,
    TerrainFinalize,
    ContentFinalize,
    RasterTextureUpload,
    TerminalState
};

enum class FrameResourcePriority {
    Preload = 0,
    Normal = 1,
    Urgent = 2
};

struct FrameResourceBudgetConfig {
    // Default per-lane network limit. It is used by terrain/content and raster
    // lanes only when their lane-specific limits are zero; it is not a global
    // sum cap across all network lanes.
    uint32_t maxNetworkRequestsPerFrame = 20;
    uint32_t maxTerrainContentNetworkRequestsPerFrame = 0;
    uint32_t maxRasterNetworkRequestsPerFrame = 0;
    // Default per-lane inflight limit with the same fallback semantics as
    // maxNetworkRequestsPerFrame.
    uint32_t maxNetworkInflight = 20;
    uint32_t maxTerrainContentNetworkInflight = 0;
    uint32_t maxRasterNetworkInflight = 0;
    uint32_t maxMainThreadFinalizesPerFrame = 1;
    uint32_t maxTerminalStateTransitionsPerFrame = 64;
    uint32_t maxRasterUploadsPerFrame = 1;
    double mainThreadTimeMs = 0.0;
    bool interactionActive = false;
    bool smoothingActive = false;
};

struct FrameResourceBudgetSnapshot {
    uint64_t frameNumber = 0;
    uint32_t networkRequestsIssued = 0;
    uint32_t terrainContentNetworkRequestsIssued = 0;
    uint32_t contentNetworkRequestsIssued = 0;
    uint32_t rasterNetworkRequestsIssued = 0;
    uint32_t mainThreadFinalizesUsed = 0;
    uint32_t terminalStateTransitionsUsed = 0;
    uint32_t rasterUploadsUsed = 0;
    uint32_t maxNetworkRequestsPerFrame = 0;
    uint32_t maxTerrainContentNetworkRequestsPerFrame = 0;
    uint32_t maxContentNetworkRequestsPerFrame = 0;
    uint32_t maxRasterNetworkRequestsPerFrame = 0;
    uint32_t maxNetworkInflight = 0;
    uint32_t maxTerrainContentNetworkInflight = 0;
    uint32_t maxContentNetworkInflight = 0;
    uint32_t maxRasterNetworkInflight = 0;
    uint32_t maxMainThreadFinalizesPerFrame = 0;
    uint32_t maxTerminalStateTransitionsPerFrame = 0;
    uint32_t maxRasterUploadsPerFrame = 0;
    double mainThreadElapsedMs = 0.0;
    double mainThreadTimeMs = 0.0;
    bool interactionActive = false;
    bool smoothingActive = false;
};

class FrameResourceBudget {
public:
    void beginFrame(uint64_t frameNumber,
                    const FrameResourceBudgetConfig& config);

    bool canIssue(FrameResourceLane lane,
                  FrameResourcePriority priority,
                  int estimatedFanout = 1) const;
    bool tryIssue(FrameResourceLane lane,
                  FrameResourcePriority priority,
                  int estimatedFanout = 1);
    bool hasNetworkInflightCapacity(FrameResourceLane lane,
                                    uint32_t currentInflight,
                                    int estimatedFanout = 1) const;
    bool hasNetworkInflightCapacity(uint32_t currentInflight,
                                    int estimatedFanout = 1) const;

    bool canFinalize(FrameResourceLane lane,
                     FrameResourcePriority priority,
                     int estimatedCostUnits = 1) const;
    bool tryFinalize(FrameResourceLane lane,
                     FrameResourcePriority priority,
                     int estimatedCostUnits = 1);

    void recordElapsed(FrameResourceLane lane, double elapsedMs);
    bool mainThreadTimeExpired() const;

    uint64_t frameNumber() const { return frameNumber_; }
    uint32_t networkRequestsIssued() const { return networkRequestsIssued_; }
    uint32_t terrainContentNetworkRequestsIssued() const {
        return terrainContentNetworkRequestsIssued_;
    }
    uint32_t contentNetworkRequestsIssued() const {
        return contentNetworkRequestsIssued_;
    }
    uint32_t rasterNetworkRequestsIssued() const {
        return rasterNetworkRequestsIssued_;
    }
    uint32_t rasterUploadsUsed() const { return rasterUploadsUsed_; }
    double mainThreadElapsedMs() const { return mainThreadElapsedMs_; }
    FrameResourceBudgetSnapshot snapshot() const;

private:
    static uint32_t positiveUnits(int estimatedUnits);
    uint32_t networkRequestLimit(FrameResourceLane lane) const;
    uint32_t networkInflightLimit(FrameResourceLane lane) const;

    uint64_t frameNumber_ = 0;
    FrameResourceBudgetConfig config_;
    uint32_t networkRequestsIssued_ = 0;
    uint32_t terrainContentNetworkRequestsIssued_ = 0;
    uint32_t contentNetworkRequestsIssued_ = 0;
    uint32_t rasterNetworkRequestsIssued_ = 0;
    uint32_t mainThreadFinalizesUsed_ = 0;
    uint32_t terminalStateTransitionsUsed_ = 0;
    uint32_t rasterUploadsUsed_ = 0;
    double mainThreadElapsedMs_ = 0.0;
};

} // namespace earth_engine
