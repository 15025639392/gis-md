#include "SceneFrameResourceArbiter.h"

#include <algorithm>
#include <limits>

namespace earth_engine {

namespace {

constexpr std::size_t toIndex(SceneFrameResourceProducer producer) {
    return static_cast<std::size_t>(producer);
}

constexpr std::size_t toIndex(SceneFrameResourceStage stage) {
    return static_cast<std::size_t>(stage);
}

} // namespace

const char* sceneFrameResourceProducerName(
    SceneFrameResourceProducer producer) {
    switch (producer) {
        case SceneFrameResourceProducer::Terrain:
            return "terrain";
        case SceneFrameResourceProducer::Raster:
            return "raster";
        case SceneFrameResourceProducer::Mvt:
            return "mvt";
        case SceneFrameResourceProducer::PageStore:
            return "pageStore";
        case SceneFrameResourceProducer::Count:
            break;
    }
    return "unknown";
}

const char* sceneFrameResourceStageName(SceneFrameResourceStage stage) {
    switch (stage) {
        case SceneFrameResourceStage::NetworkRequest:
            return "networkRequest";
        case SceneFrameResourceStage::WorkerDispatch:
            return "workerDispatch";
        case SceneFrameResourceStage::MainThreadFinalize:
            return "mainThreadFinalize";
        case SceneFrameResourceStage::GpuUpload:
            return "gpuUpload";
        case SceneFrameResourceStage::ComposeDispatch:
            return "composeDispatch";
        case SceneFrameResourceStage::Count:
            break;
    }
    return "unknown";
}

SceneFrameStageBudgetConfig& SceneFrameResourceArbiterConfig::stageBudget(
    SceneFrameResourceStage stage) {
    switch (stage) {
        case SceneFrameResourceStage::NetworkRequest:
            return networkRequest;
        case SceneFrameResourceStage::WorkerDispatch:
            return workerDispatch;
        case SceneFrameResourceStage::MainThreadFinalize:
            return mainThreadFinalize;
        case SceneFrameResourceStage::GpuUpload:
            return gpuUpload;
        case SceneFrameResourceStage::ComposeDispatch:
            return composeDispatch;
        case SceneFrameResourceStage::Count:
            break;
    }
    return networkRequest;
}

const SceneFrameStageBudgetConfig&
SceneFrameResourceArbiterConfig::stageBudget(
    SceneFrameResourceStage stage) const {
    switch (stage) {
        case SceneFrameResourceStage::NetworkRequest:
            return networkRequest;
        case SceneFrameResourceStage::WorkerDispatch:
            return workerDispatch;
        case SceneFrameResourceStage::MainThreadFinalize:
            return mainThreadFinalize;
        case SceneFrameResourceStage::GpuUpload:
            return gpuUpload;
        case SceneFrameResourceStage::ComposeDispatch:
            return composeDispatch;
        case SceneFrameResourceStage::Count:
            break;
    }
    return networkRequest;
}

const SceneFrameStageSnapshot& SceneFrameResourceArbiterSnapshot::stage(
    SceneFrameResourceStage value) const {
    return stages[toIndex(value)];
}

const SceneFrameProducerStageSnapshot&
SceneFrameResourceArbiterSnapshot::producerStage(
    SceneFrameResourceProducer producer,
    SceneFrameResourceStage stage) const {
    return producerStages[toIndex(producer)][toIndex(stage)];
}

SceneFrameResourceArbiter::ProducerLease::ProducerLease(
    SceneFrameResourceArbiter* arbiter,
    SceneFrameResourceProducer producer,
    uint64_t frameNumber,
    uint64_t frameGeneration)
    : arbiter_(arbiter),
      producer_(producer),
      frameNumber_(frameNumber),
      frameGeneration_(frameGeneration) {}

bool SceneFrameResourceArbiter::ProducerLease::valid() const {
    return arbiter_ && arbiter_->frameStarted_ &&
           arbiter_->frameGeneration_ == frameGeneration_;
}

bool SceneFrameResourceArbiter::ProducerLease::declareDemand(
    SceneFrameResourceStage stage,
    FrameResourcePriority priority,
    uint32_t units) {
    return valid() &&
           arbiter_->declareDemand(producer_, stage, priority, units);
}

bool SceneFrameResourceArbiter::ProducerLease::tryAcquire(
    SceneFrameResourceStage stage,
    FrameResourcePriority priority,
    uint32_t units) {
    return valid() &&
           arbiter_->tryAcquire(producer_, stage, priority, units);
}

uint32_t SceneFrameResourceArbiter::ProducerLease::granted(
    SceneFrameResourceStage stage,
    FrameResourcePriority priority) const {
    return valid() ? arbiter_->granted(producer_, stage, priority) : 0;
}

uint32_t SceneFrameResourceArbiter::ProducerLease::remaining(
    SceneFrameResourceStage stage,
    FrameResourcePriority priority) const {
    return valid() ? arbiter_->remaining(producer_, stage, priority) : 0;
}

void SceneFrameResourceArbiter::beginFrame(
    uint64_t frameNumber,
    const SceneFrameResourceArbiterConfig& config) {
    if (frameStarted_ && allocationsSealed_) {
        for (std::size_t producer = 0;
             producer < kSceneFrameResourceProducerCount;
             ++producer) {
            for (std::size_t stage = 0;
                 stage < kSceneFrameResourceStageCount;
                 ++stage) {
                for (std::size_t priority = 0;
                     priority < kSceneFrameResourcePriorityCount;
                     ++priority) {
                    suggestedDemand_[producer][stage][priority] =
                        saturatingAdd(
                            saturatingAdd(
                                used_[producer][stage][priority],
                                denied_[producer][stage][priority]),
                            instanceDeferred_[producer][stage][priority]);
                }
            }
        }
    }
    frameStarted_ = true;
    allocationsSealed_ = false;
    frameNumber_ = frameNumber;
    ++frameGeneration_;
    config_ = config;
    demand_ = {};
    granted_ = {};
    used_ = {};
    denied_ = {};
    instanceDeferred_ = {};
    reservedUrgentTerrainGranted_ = {};
}

SceneFrameResourceArbiter::ProducerLease SceneFrameResourceArbiter::lease(
    SceneFrameResourceProducer producer) {
    if (!frameStarted_ || producer == SceneFrameResourceProducer::Count) {
        return {};
    }
    return ProducerLease(this, producer, frameNumber_, frameGeneration_);
}

bool SceneFrameResourceArbiter::declareDemand(
    SceneFrameResourceProducer producer,
    SceneFrameResourceStage stage,
    FrameResourcePriority priority,
    uint32_t units) {
    if (!frameStarted_ || allocationsSealed_ || units == 0 ||
        producer == SceneFrameResourceProducer::Count ||
        stage == SceneFrameResourceStage::Count) {
        return false;
    }

    uint32_t& value = demand_[producerIndex(producer)][stageIndex(stage)]
                              [priorityIndex(priority)];
    value = saturatingAdd(value, units);
    return true;
}

bool SceneFrameResourceArbiter::sealAllocations() {
    if (!frameStarted_) {
        return false;
    }
    if (allocationsSealed_) {
        return true;
    }

    for (std::size_t stageValue = 0;
         stageValue < kSceneFrameResourceStageCount;
         ++stageValue) {
        const auto stage =
            static_cast<SceneFrameResourceStage>(stageValue);
        const SceneFrameStageBudgetConfig& stageConfig =
            config_.stageBudget(stage);
        uint32_t available = stageConfig.maxUnitsPerFrame;

        const std::size_t terrain =
            producerIndex(SceneFrameResourceProducer::Terrain);
        const std::size_t urgent =
            priorityIndex(FrameResourcePriority::Urgent);
        const uint32_t terrainUrgentDemand =
            demand_[terrain][stageValue][urgent];
        const uint32_t reservedGrant = std::min(
            {available,
             stageConfig.reservedUrgentTerrainUnits,
             terrainUrgentDemand});
        granted_[terrain][stageValue][urgent] = reservedGrant;
        reservedUrgentTerrainGranted_[stageValue] = reservedGrant;
        available -= reservedGrant;

        // A terrain reservation is a floor, not an implicit second turn for
        // terrain.  Do not reset the persistent cursor here: doing so every
        // frame gives the producer after terrain the remainder forever when
        // several non-terrain urgent producers are active.  If the cursor
        // happens to point at terrain, skip that already-served producer for
        // this urgent pass while preserving the cross-frame rotation.
        if (reservedGrant > 0 &&
            roundRobinCursor_[stageValue][urgent] == terrain) {
            roundRobinCursor_[stageValue][urgent] =
                (terrain + 1) % kSceneFrameResourceProducerCount;
        }

        available -= allocateRoundRobin(
            stage,
            FrameResourcePriority::Urgent,
            available);
        available -= allocateRoundRobin(
            stage,
            FrameResourcePriority::Normal,
            available);
        allocateRoundRobin(
            stage,
            FrameResourcePriority::Preload,
            available);
    }

    allocationsSealed_ = true;
    return true;
}

bool SceneFrameResourceArbiter::tryAcquire(
    SceneFrameResourceProducer producer,
    SceneFrameResourceStage stage,
    FrameResourcePriority priority,
    uint32_t units) {
    if (!frameStarted_ || !allocationsSealed_ || units == 0 ||
        producer == SceneFrameResourceProducer::Count ||
        stage == SceneFrameResourceStage::Count) {
        return false;
    }

    uint32_t& consumed = used_[producerIndex(producer)][stageIndex(stage)]
                              [priorityIndex(priority)];
    const uint32_t allocation =
        granted_[producerIndex(producer)][stageIndex(stage)]
                [priorityIndex(priority)];
    if (consumed > allocation || units > allocation - consumed) {
        noteUnservedDemand(producer, stage, priority, units);
        return false;
    }
    consumed += units;
    return true;
}

void SceneFrameResourceArbiter::noteUnservedDemand(
    SceneFrameResourceProducer producer,
    SceneFrameResourceStage stage,
    FrameResourcePriority priority,
    uint32_t units) const {
    if (!frameStarted_ || !allocationsSealed_ || units == 0 ||
        producer == SceneFrameResourceProducer::Count ||
        stage == SceneFrameResourceStage::Count) {
        return;
    }
    uint32_t& denied =
        denied_[producerIndex(producer)][stageIndex(stage)]
               [priorityIndex(priority)];
    denied = saturatingAdd(denied, units);
}

void SceneFrameResourceArbiter::noteInstanceDeferredDemand(
    SceneFrameResourceProducer producer,
    SceneFrameResourceStage stage,
    FrameResourcePriority priority,
    uint32_t units) const {
    if (!frameStarted_ || !allocationsSealed_ || units == 0 ||
        producer == SceneFrameResourceProducer::Count ||
        stage == SceneFrameResourceStage::Count) {
        return;
    }
    uint32_t& deferred =
        instanceDeferred_[producerIndex(producer)][stageIndex(stage)]
                         [priorityIndex(priority)];
    deferred = saturatingAdd(deferred, units);
}

uint32_t SceneFrameResourceArbiter::suggestedDemand(
    SceneFrameResourceProducer producer,
    SceneFrameResourceStage stage,
    FrameResourcePriority priority) const {
    if (producer == SceneFrameResourceProducer::Count ||
        stage == SceneFrameResourceStage::Count) {
        return 0;
    }
    return suggestedDemand_[producerIndex(producer)][stageIndex(stage)]
                           [priorityIndex(priority)];
}

uint32_t SceneFrameResourceArbiter::demand(
    SceneFrameResourceProducer producer,
    SceneFrameResourceStage stage,
    FrameResourcePriority priority) const {
    if (producer == SceneFrameResourceProducer::Count ||
        stage == SceneFrameResourceStage::Count) {
        return 0;
    }
    return demand_[producerIndex(producer)][stageIndex(stage)]
                  [priorityIndex(priority)];
}

uint32_t SceneFrameResourceArbiter::granted(
    SceneFrameResourceProducer producer,
    SceneFrameResourceStage stage,
    FrameResourcePriority priority) const {
    if (producer == SceneFrameResourceProducer::Count ||
        stage == SceneFrameResourceStage::Count) {
        return 0;
    }
    return granted_[producerIndex(producer)][stageIndex(stage)]
                   [priorityIndex(priority)];
}

uint32_t SceneFrameResourceArbiter::used(
    SceneFrameResourceProducer producer,
    SceneFrameResourceStage stage,
    FrameResourcePriority priority) const {
    if (producer == SceneFrameResourceProducer::Count ||
        stage == SceneFrameResourceStage::Count) {
        return 0;
    }
    return used_[producerIndex(producer)][stageIndex(stage)]
                [priorityIndex(priority)];
}

uint32_t SceneFrameResourceArbiter::remaining(
    SceneFrameResourceProducer producer,
    SceneFrameResourceStage stage,
    FrameResourcePriority priority) const {
    const uint32_t allocation = granted(producer, stage, priority);
    const uint32_t consumed = used(producer, stage, priority);
    return allocation >= consumed ? allocation - consumed : 0;
}

SceneFrameResourceArbiterSnapshot SceneFrameResourceArbiter::snapshot() const {
    SceneFrameResourceArbiterSnapshot result;
    result.frameNumber = frameNumber_;
    result.allocationsSealed = allocationsSealed_;

    for (std::size_t producer = 0;
         producer < kSceneFrameResourceProducerCount;
         ++producer) {
        for (std::size_t stage = 0;
             stage < kSceneFrameResourceStageCount;
             ++stage) {
            SceneFrameProducerStageSnapshot& producerStage =
                result.producerStages[producer][stage];
            for (std::size_t priority = 0;
                 priority < kSceneFrameResourcePriorityCount;
                 ++priority) {
                SceneFramePrioritySnapshot& prioritySnapshot =
                    producerStage.priorities[priority];
                prioritySnapshot.demand =
                    demand_[producer][stage][priority];
                prioritySnapshot.granted =
                    granted_[producer][stage][priority];
                prioritySnapshot.used =
                    used_[producer][stage][priority];
                if (allocationsSealed_) {
                    const uint32_t unallocated =
                        prioritySnapshot.demand -
                        prioritySnapshot.granted;
                    const uint32_t observedDeferred = saturatingAdd(
                        denied_[producer][stage][priority],
                        instanceDeferred_[producer][stage][priority]);
                    prioritySnapshot.deferred = std::max(
                        unallocated,
                        observedDeferred);
                }
                producerStage.demand = saturatingAdd(
                    producerStage.demand,
                    prioritySnapshot.demand);
                producerStage.granted = saturatingAdd(
                    producerStage.granted,
                    prioritySnapshot.granted);
                producerStage.used = saturatingAdd(
                    producerStage.used,
                    prioritySnapshot.used);
            }
            if (allocationsSealed_) {
                for (std::size_t priority = 0;
                     priority < kSceneFrameResourcePriorityCount;
                     ++priority) {
                    producerStage.deferred = saturatingAdd(
                        producerStage.deferred,
                        producerStage.priorities[priority].deferred);
                }
            }

            SceneFrameStageSnapshot& stageSnapshot = result.stages[stage];
            stageSnapshot.demand += producerStage.demand;
            stageSnapshot.granted += producerStage.granted;
            stageSnapshot.used += producerStage.used;
            stageSnapshot.deferred += producerStage.deferred;
            for (std::size_t priority = 0;
                 priority < kSceneFrameResourcePriorityCount;
                 ++priority) {
                const SceneFramePrioritySnapshot& producerPriority =
                    producerStage.priorities[priority];
                SceneFrameResourceTotalsSnapshot& stagePriority =
                    stageSnapshot.priorities[priority];
                stagePriority.demand += producerPriority.demand;
                stagePriority.granted += producerPriority.granted;
                stagePriority.used += producerPriority.used;
                stagePriority.deferred += producerPriority.deferred;
            }
        }
    }

    for (std::size_t stage = 0;
         stage < kSceneFrameResourceStageCount;
         ++stage) {
        const auto stageValue =
            static_cast<SceneFrameResourceStage>(stage);
        SceneFrameStageSnapshot& stageSnapshot = result.stages[stage];
        const SceneFrameStageBudgetConfig& stageConfig =
            config_.stageBudget(stageValue);
        stageSnapshot.budget = stageConfig.maxUnitsPerFrame;
        stageSnapshot.reservedUrgentTerrain =
            stageConfig.reservedUrgentTerrainUnits;
        stageSnapshot.reservedUrgentTerrainGranted =
            reservedUrgentTerrainGranted_[stage];

        result.totals.demand += stageSnapshot.demand;
        result.totals.granted += stageSnapshot.granted;
        result.totals.used += stageSnapshot.used;
        result.totals.deferred += stageSnapshot.deferred;
    }

    return result;
}

std::size_t SceneFrameResourceArbiter::producerIndex(
    SceneFrameResourceProducer producer) {
    return static_cast<std::size_t>(producer);
}

std::size_t SceneFrameResourceArbiter::stageIndex(
    SceneFrameResourceStage stage) {
    return static_cast<std::size_t>(stage);
}

std::size_t SceneFrameResourceArbiter::priorityIndex(
    FrameResourcePriority priority) {
    return static_cast<std::size_t>(priority);
}

uint32_t SceneFrameResourceArbiter::saturatingAdd(uint32_t left,
                                                  uint32_t right) {
    const uint32_t maximum = std::numeric_limits<uint32_t>::max();
    return right > maximum - left ? maximum : left + right;
}

uint32_t SceneFrameResourceArbiter::allocateRoundRobin(
    SceneFrameResourceStage stage,
    FrameResourcePriority priority,
    uint32_t available) {
    if (available == 0) {
        return 0;
    }

    const std::size_t stageValue = stageIndex(stage);
    const std::size_t priorityValue = priorityIndex(priority);
    std::size_t& cursor = roundRobinCursor_[stageValue][priorityValue];
    uint32_t allocated = 0;

    while (allocated < available) {
        bool foundDemand = false;
        for (std::size_t offset = 0;
             offset < kSceneFrameResourceProducerCount;
             ++offset) {
            const std::size_t producer =
                (cursor + offset) % kSceneFrameResourceProducerCount;
            const uint32_t requested =
                demand_[producer][stageValue][priorityValue];
            uint32_t& producerGrant =
                granted_[producer][stageValue][priorityValue];
            if (producerGrant >= requested) {
                continue;
            }

            ++producerGrant;
            ++allocated;
            cursor = (producer + 1) % kSceneFrameResourceProducerCount;
            foundDemand = true;
            break;
        }
        if (!foundDemand) {
            break;
        }
    }

    return allocated;
}

} // namespace earth_engine
