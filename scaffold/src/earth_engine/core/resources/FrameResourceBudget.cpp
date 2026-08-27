#include "FrameResourceBudget.h"

#include "SceneFrameResourceArbiter.h"

#include <algorithm>

namespace earth_engine {

namespace {

SceneFrameResourceProducer producerForLane(FrameResourceLane lane) {
    switch (lane) {
        case FrameResourceLane::RasterRequest:
        case FrameResourceLane::RasterTextureUpload:
            return SceneFrameResourceProducer::Raster;
        case FrameResourceLane::TerrainRequest:
        case FrameResourceLane::ContentRequest:
        case FrameResourceLane::TerrainFinalize:
        case FrameResourceLane::ContentFinalize:
        case FrameResourceLane::TerminalState:
            return SceneFrameResourceProducer::Terrain;
    }
    return SceneFrameResourceProducer::Terrain;
}

SceneFrameResourceStage stageForLane(FrameResourceLane lane) {
    switch (lane) {
        case FrameResourceLane::TerrainRequest:
        case FrameResourceLane::ContentRequest:
        case FrameResourceLane::RasterRequest:
            return SceneFrameResourceStage::NetworkRequest;
        case FrameResourceLane::TerrainFinalize:
        case FrameResourceLane::ContentFinalize:
        case FrameResourceLane::TerminalState:
            return SceneFrameResourceStage::MainThreadFinalize;
        case FrameResourceLane::RasterTextureUpload:
            return SceneFrameResourceStage::GpuUpload;
    }
    return SceneFrameResourceStage::NetworkRequest;
}

FrameResourcePriority scenePriorityForLane(
    FrameResourceLane lane,
    FrameResourcePriority priority) {
    // Preserve the local queue's priority class at Scene scope. Terrain has
    // an additional urgent floor, but that floor must not silently demote an
    // urgent raster (or future producer) request into the normal round-robin
    // lane; otherwise the unified arbiter cannot express the caller's
    // visibility contract.
    (void)lane;
    return priority;
}

} // namespace

void FrameResourceBudget::attachSceneArbiter(
    SceneFrameResourceArbiter* arbiter) {
    sceneArbiter_ = arbiter;
}

void FrameResourceBudget::beginFrame(
    uint64_t frameNumber,
    const FrameResourceBudgetConfig& config) {
    frameNumber_ = frameNumber;
    config_ = config;
    networkRequestsIssued_ = 0;
    terrainContentNetworkRequestsIssued_ = 0;
    contentNetworkRequestsIssued_ = 0;
    rasterNetworkRequestsIssued_ = 0;
    mainThreadFinalizesUsed_ = 0;
    terminalStateTransitionsUsed_ = 0;
    rasterUploadsUsed_ = 0;
    rasterOverlayMappingsUsed_ = 0;
    mainThreadElapsedMs_ = 0.0;
    rasterOverlayMappingElapsedMs_ = 0.0;
}

bool FrameResourceBudget::canIssue(FrameResourceLane lane,
                                   FrameResourcePriority priority,
                                   int estimatedFanout) const {
    // I-P2:Urgent 每帧可超额 reserved 个(预算紧张帧不被 preload 占满饿死)。
    // 普通/Preload 仍按 limit 严格限制。
    const uint32_t urgentOverflow =
        priority == FrameResourcePriority::Urgent
            ? config_.reservedUrgentNetworkRequestsPerFrame
            : 0;
    if (sceneArbiter_ && sceneArbiter_->allocationsSealed()) {
        const auto producer = producerForLane(lane);
        const auto stage = stageForLane(lane);
        if (sceneArbiter_->remaining(
                producer, stage, scenePriorityForLane(lane, priority)) <
            positiveUnits(estimatedFanout)) {
            sceneArbiter_->noteUnservedDemand(
                producer,
                stage,
                scenePriorityForLane(lane, priority),
                positiveUnits(estimatedFanout));
            return false;
        }
    }
    switch (lane) {
        case FrameResourceLane::TerrainRequest:
            return terrainContentNetworkRequestsIssued_ +
                       positiveUnits(estimatedFanout) <=
                   networkRequestLimit(lane) + urgentOverflow;
        case FrameResourceLane::ContentRequest:
            return terrainContentNetworkRequestsIssued_ +
                       positiveUnits(estimatedFanout) <=
                   networkRequestLimit(lane) + urgentOverflow;
        case FrameResourceLane::RasterRequest:
            return rasterNetworkRequestsIssued_ + positiveUnits(estimatedFanout) <=
                   networkRequestLimit(lane) + urgentOverflow;
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
    if (sceneArbiter_ && sceneArbiter_->allocationsSealed()) {
        if (!sceneArbiter_->tryAcquire(
                producerForLane(lane), stageForLane(lane),
                scenePriorityForLane(lane, priority),
                positiveUnits(estimatedFanout))) {
            return false;
        }
    }
    switch (lane) {
        case FrameResourceLane::TerrainRequest:
            terrainContentNetworkRequestsIssued_ +=
                positiveUnits(estimatedFanout);
            networkRequestsIssued_ += positiveUnits(estimatedFanout);
            break;
        case FrameResourceLane::ContentRequest:
            contentNetworkRequestsIssued_ += positiveUnits(estimatedFanout);
            terrainContentNetworkRequestsIssued_ +=
                positiveUnits(estimatedFanout);
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
                                      FrameResourcePriority priority,
                                      int estimatedCostUnits) const {
    const uint32_t units = positiveUnits(estimatedCostUnits);
    if (mainThreadTimeExpired()) {
        return false;
    }
    if (sceneArbiter_ && sceneArbiter_->allocationsSealed()) {
        if (sceneArbiter_->remaining(
                producerForLane(lane), stageForLane(lane),
                scenePriorityForLane(lane, priority)) <
            positiveUnits(estimatedCostUnits)) {
            sceneArbiter_->noteUnservedDemand(
                producerForLane(lane),
                stageForLane(lane),
                scenePriorityForLane(lane, priority),
                positiveUnits(estimatedCostUnits));
            return false;
        }
    }
    // ⚠️ finalize/upload 是**主线程工作**,不参与 I-P2 的 urgent 超额:
    //   - 平滑/交互期主线程时间预算必须守住(即使 urgent)——设计意图见
    //     TileFrameResourceBudgetPlannerTest.SmoothingConservesMainThreadWork;
    //   - finalize 队列(takeHighestPriorityUpload)本身按优先级取队首,
    //     urgent 排在 preload 前,预算 1 时先拿名额,不存在被饿死。
    // I-P2 的 urgent 超额只作用于 canIssue(异步网络请求,超额不阻塞主线程)。

    switch (lane) {
        case FrameResourceLane::RasterTextureUpload:
            return rasterUploadsUsed_ + units <= config_.maxRasterUploadsPerFrame;
        case FrameResourceLane::TerrainFinalize:
        case FrameResourceLane::ContentFinalize:
            return mainThreadFinalizesUsed_ + units <=
                   config_.maxMainThreadFinalizesPerFrame;
        case FrameResourceLane::TerminalState:
            return terminalStateTransitionsUsed_ + units <=
                   config_.maxTerminalStateTransitionsPerFrame;
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

    if (sceneArbiter_ && sceneArbiter_->allocationsSealed()) {
        if (!sceneArbiter_->tryAcquire(
                producerForLane(lane), stageForLane(lane),
                scenePriorityForLane(lane, priority),
                positiveUnits(estimatedCostUnits))) {
            return false;
        }
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
            terminalStateTransitionsUsed_ += units;
            break;
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

bool FrameResourceBudget::canStartRasterOverlayMapping() const {
    if (config_.maxRasterOverlayMappingsPerFrame == 0 ||
        rasterOverlayMappingsUsed_ >=
            config_.maxRasterOverlayMappingsPerFrame) {
        return false;
    }
    if (config_.rasterOverlayMappingTimeMs > 0.0 &&
        rasterOverlayMappingElapsedMs_ >= config_.rasterOverlayMappingTimeMs) {
        return false;
    }
    if (!sceneArbiter_ || !sceneArbiter_->allocationsSealed()) {
        return true;
    }
    if (sceneArbiter_->remaining(
            SceneFrameResourceProducer::Raster,
            SceneFrameResourceStage::MainThreadFinalize,
            FrameResourcePriority::Normal) > 0) {
        return true;
    }
    sceneArbiter_->noteUnservedDemand(
        SceneFrameResourceProducer::Raster,
        SceneFrameResourceStage::MainThreadFinalize,
        FrameResourcePriority::Normal);
    return false;
}

bool FrameResourceBudget::tryStartRasterOverlayMapping() {
    if (!canStartRasterOverlayMapping()) {
        return false;
    }
    if (sceneArbiter_ && sceneArbiter_->allocationsSealed() &&
        !sceneArbiter_->tryAcquire(
            SceneFrameResourceProducer::Raster,
            SceneFrameResourceStage::MainThreadFinalize,
            FrameResourcePriority::Normal)) {
        return false;
    }
    ++rasterOverlayMappingsUsed_;
    return true;
}

void FrameResourceBudget::recordRasterOverlayMappingElapsed(double elapsedMs) {
    rasterOverlayMappingElapsedMs_ += std::max(0.0, elapsedMs);
}

FrameResourceBudgetSnapshot FrameResourceBudget::snapshot() const {
    FrameResourceBudgetSnapshot snapshot;
    snapshot.frameNumber = frameNumber_;
    snapshot.networkRequestsIssued = networkRequestsIssued_;
    snapshot.terrainContentNetworkRequestsIssued =
        terrainContentNetworkRequestsIssued_;
    snapshot.contentNetworkRequestsIssued = contentNetworkRequestsIssued_;
    snapshot.rasterNetworkRequestsIssued = rasterNetworkRequestsIssued_;
    snapshot.mainThreadFinalizesUsed = mainThreadFinalizesUsed_;
    snapshot.terminalStateTransitionsUsed = terminalStateTransitionsUsed_;
    snapshot.rasterUploadsUsed = rasterUploadsUsed_;
    snapshot.rasterOverlayMappingsUsed = rasterOverlayMappingsUsed_;
    snapshot.maxNetworkRequestsPerFrame =
        config_.maxNetworkRequestsPerFrame;
    snapshot.maxTerrainContentNetworkRequestsPerFrame =
        networkRequestLimit(FrameResourceLane::TerrainRequest);
    snapshot.maxContentNetworkRequestsPerFrame =
        networkRequestLimit(FrameResourceLane::ContentRequest);
    snapshot.maxRasterNetworkRequestsPerFrame =
        networkRequestLimit(FrameResourceLane::RasterRequest);
    snapshot.maxNetworkInflight = config_.maxNetworkInflight;
    snapshot.maxTerrainContentNetworkInflight =
        networkInflightLimit(FrameResourceLane::TerrainRequest);
    snapshot.maxContentNetworkInflight =
        networkInflightLimit(FrameResourceLane::ContentRequest);
    snapshot.maxRasterNetworkInflight =
        networkInflightLimit(FrameResourceLane::RasterRequest);
    snapshot.maxMainThreadFinalizesPerFrame =
        config_.maxMainThreadFinalizesPerFrame;
    snapshot.maxTerminalStateTransitionsPerFrame =
        config_.maxTerminalStateTransitionsPerFrame;
    snapshot.maxRasterUploadsPerFrame = config_.maxRasterUploadsPerFrame;
    snapshot.reservedUrgentNetworkRequestsPerFrame =
        config_.reservedUrgentNetworkRequestsPerFrame;
    snapshot.maxRasterOverlayMappingsPerFrame =
        config_.maxRasterOverlayMappingsPerFrame;
    snapshot.rasterOverlayMappingElapsedMs =
        rasterOverlayMappingElapsedMs_;
    snapshot.rasterOverlayMappingTimeMs =
        config_.rasterOverlayMappingTimeMs;
    snapshot.mainThreadElapsedMs = mainThreadElapsedMs_;
    snapshot.mainThreadTimeMs = config_.mainThreadTimeMs;
    snapshot.interactionActive = config_.interactionActive;
    snapshot.smoothingActive = config_.smoothingActive;
    return snapshot;
}

uint32_t FrameResourceBudget::positiveUnits(int estimatedUnits) {
    return static_cast<uint32_t>(std::max(1, estimatedUnits));
}

uint32_t FrameResourceBudget::networkRequestLimit(FrameResourceLane lane) const {
    switch (lane) {
        case FrameResourceLane::TerrainRequest:
            return config_.maxTerrainContentNetworkRequestsPerFrame > 0
                       ? config_.maxTerrainContentNetworkRequestsPerFrame
                       : config_.maxNetworkRequestsPerFrame;
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
            return config_.maxTerrainContentNetworkInflight > 0
                       ? config_.maxTerrainContentNetworkInflight
                       : config_.maxNetworkInflight;
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
