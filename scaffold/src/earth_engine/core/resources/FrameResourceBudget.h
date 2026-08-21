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
    // I-P2:每帧为 Urgent 保留的超额名额。预算紧张帧(普通请求已占满 limit)时
    // Urgent 仍可突破 limit 发出/完成 reserved 个 —— 近景可见瓦片不被 preload
    // 占满而 Blocked(帧级延迟,HTTP 层有并发上限兜底,不会失控)。
    // 对齐 curl 调度器 kReservedUrgentSlots=2 的保留精神;这里用"超额"而非
    // "普通让位",避免 limit 小的配置下普通请求被压到 0。
    // ⚠️ 只作用于 canIssue(异步网络请求);canFinalize 是主线程工作,平滑期
    // 必须限流,不参与超额(见 SmoothingConservesMainThreadWork)。
    uint32_t reservedUrgentNetworkRequestsPerFrame = 2;
    // First-time geometry-to-raster mapping is synchronous CPU work performed
    // before any raster request can be issued. Keep it independently bounded so
    // visible imagery can start ahead of geometry without restoring the former
    // not-Done mapping flood. Cost is ~0.15ms/mapping on-device; the count cap
    // was starving imagery (notReady backlog ~200 drained at only 4/frame while
    // the network could absorb far more), so the per-frame time budget is the
    // real limiter and the count is set above network throughput.
    uint32_t maxRasterOverlayMappingsPerFrame = 16;
    double rasterOverlayMappingTimeMs = 2.0;
    double mainThreadTimeMs = 0.0;
    bool interactionActive = false;
    bool smoothingActive = false;
    // cullRequestsWhileMoving(cesium-js):运动期跳过快速划过屏幕的瓦片请求。
    // 与其它每帧闸门同属帧级资源门控状态,随预算配置一起下发到 load scheduler。
    bool cullRequestsWhileMoving = false;
    double cullRequestsWhileMovingMultiplier = 60.0;
    double cameraPositionDeltaMagnitude = 0.0;
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
    uint32_t rasterOverlayMappingsUsed = 0;
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
    uint32_t reservedUrgentNetworkRequestsPerFrame = 0;
    uint32_t maxRasterOverlayMappingsPerFrame = 0;
    double rasterOverlayMappingElapsedMs = 0.0;
    double rasterOverlayMappingTimeMs = 0.0;
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
    bool canStartRasterOverlayMapping() const;
    bool tryStartRasterOverlayMapping();
    void recordRasterOverlayMappingElapsed(double elapsedMs);

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
    uint32_t rasterOverlayMappingsUsed() const {
        return rasterOverlayMappingsUsed_;
    }
    double rasterOverlayMappingElapsedMs() const {
        return rasterOverlayMappingElapsedMs_;
    }
    double mainThreadElapsedMs() const { return mainThreadElapsedMs_; }
    bool cullRequestsWhileMoving() const {
        return config_.cullRequestsWhileMoving;
    }
    double cullRequestsWhileMovingMultiplier() const {
        return config_.cullRequestsWhileMovingMultiplier;
    }
    double cameraPositionDeltaMagnitude() const {
        return config_.cameraPositionDeltaMagnitude;
    }
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
    uint32_t rasterOverlayMappingsUsed_ = 0;
    double mainThreadElapsedMs_ = 0.0;
    double rasterOverlayMappingElapsedMs_ = 0.0;
};

} // namespace earth_engine
