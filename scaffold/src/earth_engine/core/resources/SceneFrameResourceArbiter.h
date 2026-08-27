#pragma once

#include "FrameResourceBudget.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace earth_engine {

// A producer identifies the subsystem that owns work. Storage and cache
// implementations remain independent; only their per-frame resource usage is
// arbitrated here.
enum class SceneFrameResourceProducer : uint8_t {
    Terrain = 0,
    Raster,
    Mvt,
    PageStore,
    Count
};

// Stages are intentionally resource-shaped rather than implementation-shaped.
// Work from different producers competes only when it consumes the same kind
// of frame resource.
enum class SceneFrameResourceStage : uint8_t {
    NetworkRequest = 0,
    WorkerDispatch,
    MainThreadFinalize,
    GpuUpload,
    ComposeDispatch,
    Count
};

constexpr std::size_t kSceneFrameResourceProducerCount =
    static_cast<std::size_t>(SceneFrameResourceProducer::Count);
constexpr std::size_t kSceneFrameResourceStageCount =
    static_cast<std::size_t>(SceneFrameResourceStage::Count);
constexpr std::size_t kSceneFrameResourcePriorityCount = 3;

const char* sceneFrameResourceProducerName(
    SceneFrameResourceProducer producer);
const char* sceneFrameResourceStageName(SceneFrameResourceStage stage);

struct SceneFrameStageBudgetConfig {
    uint32_t maxUnitsPerFrame = 0;

    // These units stay available to visible/urgent terrain while allocations
    // are being sealed. Unused reservation is redistributed immediately from
    // declared demand, so it never strands capacity for the rest of the frame.
    uint32_t reservedUrgentTerrainUnits = 0;
};

struct SceneFrameResourceArbiterConfig {
    SceneFrameStageBudgetConfig networkRequest{20, 2};
    SceneFrameStageBudgetConfig workerDispatch{8, 1};
    SceneFrameStageBudgetConfig mainThreadFinalize{4, 1};
    SceneFrameStageBudgetConfig gpuUpload{4, 0};
    SceneFrameStageBudgetConfig composeDispatch{2, 0};

    SceneFrameStageBudgetConfig& stageBudget(SceneFrameResourceStage stage);
    const SceneFrameStageBudgetConfig& stageBudget(
        SceneFrameResourceStage stage) const;
};

struct SceneFramePrioritySnapshot {
    uint32_t demand = 0;
    uint32_t granted = 0;
    uint32_t used = 0;
    uint32_t deferred = 0;
};

struct SceneFrameProducerStageSnapshot : SceneFramePrioritySnapshot {
    std::array<SceneFramePrioritySnapshot,
               kSceneFrameResourcePriorityCount>
        priorities{};
};

struct SceneFrameResourceTotalsSnapshot {
    uint64_t demand = 0;
    uint64_t granted = 0;
    uint64_t used = 0;
    uint64_t deferred = 0;
};

struct SceneFrameStageSnapshot {
    uint32_t budget = 0;
    uint32_t reservedUrgentTerrain = 0;
    uint32_t reservedUrgentTerrainGranted = 0;
    uint64_t demand = 0;
    uint64_t granted = 0;
    uint64_t used = 0;
    uint64_t deferred = 0;
    std::array<SceneFrameResourceTotalsSnapshot,
               kSceneFrameResourcePriorityCount>
        priorities{};
};

struct SceneFrameResourceArbiterSnapshot {
    uint64_t frameNumber = 0;
    bool allocationsSealed = false;
    SceneFrameResourceTotalsSnapshot totals;
    std::array<SceneFrameStageSnapshot, kSceneFrameResourceStageCount> stages{};
    std::array<
        std::array<SceneFrameProducerStageSnapshot,
                   kSceneFrameResourceStageCount>,
        kSceneFrameResourceProducerCount>
        producerStages{};

    const SceneFrameStageSnapshot& stage(SceneFrameResourceStage value) const;
    const SceneFrameProducerStageSnapshot& producerStage(
        SceneFrameResourceProducer producer,
        SceneFrameResourceStage stage) const;
};

// Scene/Engine-owned, main-thread frame arbiter. The intended lifecycle is:
//   beginFrame -> declareDemand (all producers) -> sealAllocations -> consume.
// Keeping allocation separate from consumption makes the result independent of
// Scene traversal order and prevents an early producer from borrowing a later
// producer's grant.
class SceneFrameResourceArbiter {
public:
    class ProducerLease {
    public:
        ProducerLease() = default;

        bool valid() const;
        uint64_t frameNumber() const { return frameNumber_; }
        SceneFrameResourceProducer producer() const { return producer_; }

        bool declareDemand(SceneFrameResourceStage stage,
                           FrameResourcePriority priority,
                           uint32_t units = 1);
        bool tryAcquire(SceneFrameResourceStage stage,
                        FrameResourcePriority priority,
                        uint32_t units = 1);
        uint32_t granted(SceneFrameResourceStage stage,
                         FrameResourcePriority priority) const;
        uint32_t remaining(SceneFrameResourceStage stage,
                           FrameResourcePriority priority) const;

    private:
        friend class SceneFrameResourceArbiter;
        ProducerLease(SceneFrameResourceArbiter* arbiter,
                      SceneFrameResourceProducer producer,
                      uint64_t frameNumber,
                      uint64_t frameGeneration);

        SceneFrameResourceArbiter* arbiter_ = nullptr;
        SceneFrameResourceProducer producer_ =
            SceneFrameResourceProducer::Terrain;
        uint64_t frameNumber_ = 0;
        uint64_t frameGeneration_ = 0;
    };

    void beginFrame(uint64_t frameNumber,
                    const SceneFrameResourceArbiterConfig& config);

    ProducerLease lease(SceneFrameResourceProducer producer);

    bool declareDemand(SceneFrameResourceProducer producer,
                       SceneFrameResourceStage stage,
                       FrameResourcePriority priority,
                       uint32_t units = 1);

    // Idempotent. Returns false only when no frame has been started.
    bool sealAllocations();

    bool tryAcquire(SceneFrameResourceProducer producer,
                    SceneFrameResourceStage stage,
                    FrameResourcePriority priority,
                    uint32_t units = 1);

    // Record real work that reached a sealed lane but could not be admitted.
    // The observation updates this frame's deferred snapshot and becomes a
    // demand hint for the next frame; it does not change sealed grants.
    void noteUnservedDemand(SceneFrameResourceProducer producer,
                            SceneFrameResourceStage stage,
                            FrameResourcePriority priority,
                            uint32_t units = 1) const;

    // A nested producer scheduler may intentionally cap one instance below
    // the producer's aggregate remaining grant to preserve instance fairness.
    // Record candidate work deferred by that local share separately from a
    // global admission denial; it still feeds next-frame demand and snapshot
    // observability.
    void noteInstanceDeferredDemand(
        SceneFrameResourceProducer producer,
        SceneFrameResourceStage stage,
        FrameResourcePriority priority,
        uint32_t units = 1) const;

    // Previous-frame used + admission-denied work. Scene uses this before
    // sealing to avoid granting fixed full budgets to enabled-but-idle systems.
    uint32_t suggestedDemand(SceneFrameResourceProducer producer,
                             SceneFrameResourceStage stage,
                             FrameResourcePriority priority) const;

    uint32_t demand(SceneFrameResourceProducer producer,
                    SceneFrameResourceStage stage,
                    FrameResourcePriority priority) const;
    uint32_t granted(SceneFrameResourceProducer producer,
                     SceneFrameResourceStage stage,
                     FrameResourcePriority priority) const;
    uint32_t used(SceneFrameResourceProducer producer,
                  SceneFrameResourceStage stage,
                  FrameResourcePriority priority) const;
    uint32_t remaining(SceneFrameResourceProducer producer,
                       SceneFrameResourceStage stage,
                       FrameResourcePriority priority) const;

    uint64_t frameNumber() const { return frameNumber_; }
    bool allocationsSealed() const { return allocationsSealed_; }
    SceneFrameResourceArbiterSnapshot snapshot() const;

private:
    using PriorityCounters =
        std::array<uint32_t, kSceneFrameResourcePriorityCount>;
    using StagePriorityCounters =
        std::array<PriorityCounters, kSceneFrameResourceStageCount>;
    using ProducerStagePriorityCounters =
        std::array<StagePriorityCounters,
                   kSceneFrameResourceProducerCount>;

    static std::size_t producerIndex(SceneFrameResourceProducer producer);
    static std::size_t stageIndex(SceneFrameResourceStage stage);
    static std::size_t priorityIndex(FrameResourcePriority priority);
    static uint32_t saturatingAdd(uint32_t left, uint32_t right);

    uint32_t allocateRoundRobin(SceneFrameResourceStage stage,
                                FrameResourcePriority priority,
                                uint32_t available);

    bool frameStarted_ = false;
    bool allocationsSealed_ = false;
    uint64_t frameNumber_ = 0;
    uint64_t frameGeneration_ = 0;
    SceneFrameResourceArbiterConfig config_;
    ProducerStagePriorityCounters demand_{};
    ProducerStagePriorityCounters granted_{};
    ProducerStagePriorityCounters used_{};
    mutable ProducerStagePriorityCounters denied_{};
    mutable ProducerStagePriorityCounters instanceDeferred_{};
    ProducerStagePriorityCounters suggestedDemand_{};
    std::array<uint32_t, kSceneFrameResourceStageCount>
        reservedUrgentTerrainGranted_{};

    // Cursors intentionally survive beginFrame. A producer that continuously
    // declares normal demand therefore rotates across frames even when a stage
    // budget is smaller than the number of active producers.
    std::array<
        std::array<std::size_t, kSceneFrameResourcePriorityCount>,
        kSceneFrameResourceStageCount>
        roundRobinCursor_{};
};

} // namespace earth_engine
