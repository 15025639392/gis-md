#include <gtest/gtest.h>

#include "earth_engine/core/resources/SceneFrameResourceArbiter.h"

using namespace earth_engine;

namespace {

SceneFrameResourceArbiterConfig makeConfig(uint32_t network,
                                           uint32_t workers = 8,
                                           uint32_t finalizes = 4,
                                           uint32_t uploads = 4,
                                           uint32_t compose = 2) {
    SceneFrameResourceArbiterConfig config;
    config.networkRequest.maxUnitsPerFrame = network;
    config.workerDispatch.maxUnitsPerFrame = workers;
    config.mainThreadFinalize.maxUnitsPerFrame = finalizes;
    config.gpuUpload.maxUnitsPerFrame = uploads;
    config.composeDispatch.maxUnitsPerFrame = compose;
    config.networkRequest.reservedUrgentTerrainUnits = 0;
    config.workerDispatch.reservedUrgentTerrainUnits = 0;
    config.mainThreadFinalize.reservedUrgentTerrainUnits = 0;
    config.gpuUpload.reservedUrgentTerrainUnits = 0;
    config.composeDispatch.reservedUrgentTerrainUnits = 0;
    return config;
}

} // namespace

TEST(SceneFrameResourceArbiterTest, TerrainUrgentReservationIsAllocatedFirst) {
    SceneFrameResourceArbiterConfig config = makeConfig(1);
    SceneFrameResourceArbiter arbiter;
    arbiter.beginFrame(11, config);
    EXPECT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Terrain,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Urgent));
    EXPECT_TRUE(arbiter.sealAllocations());
    // Advance the urgent round-robin cursor to Raster.
    EXPECT_EQ(1u, arbiter.granted(SceneFrameResourceProducer::Terrain,
                                  SceneFrameResourceStage::NetworkRequest,
                                  FrameResourcePriority::Urgent));

    config.networkRequest.reservedUrgentTerrainUnits = 1;
    arbiter.beginFrame(12, config);
    EXPECT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Terrain,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Urgent,
        1));
    EXPECT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Raster,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Urgent,
        1));
    EXPECT_TRUE(arbiter.sealAllocations());
    EXPECT_EQ(1u, arbiter.granted(SceneFrameResourceProducer::Terrain,
                                   SceneFrameResourceStage::NetworkRequest,
                                   FrameResourcePriority::Urgent));
    EXPECT_EQ(0u, arbiter.granted(SceneFrameResourceProducer::Raster,
                                  SceneFrameResourceStage::NetworkRequest,
                                  FrameResourcePriority::Urgent));

    const auto snapshot = arbiter.snapshot();
    EXPECT_EQ(1u, snapshot.stage(SceneFrameResourceStage::NetworkRequest).granted);
    EXPECT_EQ(1u, snapshot.stage(SceneFrameResourceStage::NetworkRequest)
                       .reservedUrgentTerrainGranted);
    EXPECT_EQ(1u, snapshot.producerStage(
                           SceneFrameResourceProducer::Raster,
                           SceneFrameResourceStage::NetworkRequest)
                       .deferred);
}

TEST(SceneFrameResourceArbiterTest, UnusedTerrainReservationIsRedistributed) {
    SceneFrameResourceArbiterConfig config = makeConfig(2);
    config.networkRequest.reservedUrgentTerrainUnits = 2;

    SceneFrameResourceArbiter arbiter;
    arbiter.beginFrame(1, config);
    EXPECT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Mvt,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal,
        2));
    EXPECT_TRUE(arbiter.sealAllocations());

    EXPECT_EQ(2u, arbiter.granted(SceneFrameResourceProducer::Mvt,
                                  SceneFrameResourceStage::NetworkRequest,
                                  FrameResourcePriority::Normal));
    EXPECT_EQ(0u, arbiter.snapshot()
                      .stage(SceneFrameResourceStage::NetworkRequest)
                      .reservedUrgentTerrainGranted);
}

TEST(SceneFrameResourceArbiterTest, TerrainReservationDoesNotTakeSecondUrgentTurn) {
    SceneFrameResourceArbiterConfig config = makeConfig(2);
    config.networkRequest.reservedUrgentTerrainUnits = 1;

    SceneFrameResourceArbiter arbiter;
    arbiter.beginFrame(1, config);
    EXPECT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Terrain,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Urgent,
        2));
    EXPECT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Raster,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Urgent,
        1));
    EXPECT_TRUE(arbiter.sealAllocations());

    EXPECT_EQ(1u, arbiter.granted(SceneFrameResourceProducer::Terrain,
                                  SceneFrameResourceStage::NetworkRequest,
                                  FrameResourcePriority::Urgent));
    EXPECT_EQ(1u, arbiter.granted(SceneFrameResourceProducer::Raster,
                                  SceneFrameResourceStage::NetworkRequest,
                                  FrameResourcePriority::Urgent));
}

TEST(SceneFrameResourceArbiterTest,
     TerrainReservationPreservesUrgentRoundRobinAcrossFrames) {
    SceneFrameResourceArbiterConfig config = makeConfig(2);
    config.networkRequest.reservedUrgentTerrainUnits = 1;
    SceneFrameResourceArbiter arbiter;

    const auto runFrame = [&](uint64_t frame) {
        arbiter.beginFrame(frame, config);
        for (const auto producer : {SceneFrameResourceProducer::Terrain,
                                    SceneFrameResourceProducer::Raster,
                                    SceneFrameResourceProducer::Mvt}) {
            EXPECT_TRUE(arbiter.declareDemand(
                producer,
                SceneFrameResourceStage::NetworkRequest,
                FrameResourcePriority::Urgent,
                2));
        }
        EXPECT_TRUE(arbiter.sealAllocations());
    };

    runFrame(1);
    EXPECT_EQ(1u, arbiter.granted(
                      SceneFrameResourceProducer::Raster,
                      SceneFrameResourceStage::NetworkRequest,
                      FrameResourcePriority::Urgent));
    runFrame(2);
    EXPECT_EQ(1u, arbiter.granted(
                      SceneFrameResourceProducer::Mvt,
                      SceneFrameResourceStage::NetworkRequest,
                      FrameResourcePriority::Urgent));
    runFrame(3);
    EXPECT_EQ(2u, arbiter.granted(
                      SceneFrameResourceProducer::Terrain,
                      SceneFrameResourceStage::NetworkRequest,
                      FrameResourcePriority::Urgent));
}

TEST(SceneFrameResourceArbiterTest, NormalDemandRotatesAcrossFrames) {
    SceneFrameResourceArbiterConfig config = makeConfig(1);
    SceneFrameResourceArbiter arbiter;

    const auto request = [&](uint64_t frame) {
        arbiter.beginFrame(frame, config);
        EXPECT_TRUE(arbiter.declareDemand(
            SceneFrameResourceProducer::Terrain,
            SceneFrameResourceStage::NetworkRequest,
            FrameResourcePriority::Normal));
        EXPECT_TRUE(arbiter.declareDemand(
            SceneFrameResourceProducer::Raster,
            SceneFrameResourceStage::NetworkRequest,
            FrameResourcePriority::Normal));
        EXPECT_TRUE(arbiter.declareDemand(
            SceneFrameResourceProducer::Mvt,
            SceneFrameResourceStage::NetworkRequest,
            FrameResourcePriority::Normal));
        EXPECT_TRUE(arbiter.declareDemand(
            SceneFrameResourceProducer::PageStore,
            SceneFrameResourceStage::NetworkRequest,
            FrameResourcePriority::Normal));
        EXPECT_TRUE(arbiter.sealAllocations());
    };

    request(1);
    EXPECT_EQ(1u, arbiter.granted(SceneFrameResourceProducer::Terrain,
                                   SceneFrameResourceStage::NetworkRequest,
                                   FrameResourcePriority::Normal));
    request(2);
    EXPECT_EQ(1u, arbiter.granted(SceneFrameResourceProducer::Raster,
                                   SceneFrameResourceStage::NetworkRequest,
                                   FrameResourcePriority::Normal));
    request(3);
    EXPECT_EQ(1u, arbiter.granted(SceneFrameResourceProducer::Mvt,
                                   SceneFrameResourceStage::NetworkRequest,
                                   FrameResourcePriority::Normal));
    request(4);
    EXPECT_EQ(1u, arbiter.granted(SceneFrameResourceProducer::PageStore,
                                  SceneFrameResourceStage::NetworkRequest,
                                  FrameResourcePriority::Normal));
    request(5);
    EXPECT_EQ(1u, arbiter.granted(SceneFrameResourceProducer::Terrain,
                                  SceneFrameResourceStage::NetworkRequest,
                                  FrameResourcePriority::Normal));
}

TEST(SceneFrameResourceArbiterTest, StageBudgetsAreIndependent) {
    SceneFrameResourceArbiterConfig config = makeConfig(1, 2, 1, 1, 1);
    SceneFrameResourceArbiter arbiter;
    arbiter.beginFrame(20, config);

    EXPECT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Terrain,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal,
        2));
    EXPECT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Raster,
        SceneFrameResourceStage::GpuUpload,
        FrameResourcePriority::Normal,
        2));
    EXPECT_TRUE(arbiter.sealAllocations());

    EXPECT_EQ(1u, arbiter.granted(SceneFrameResourceProducer::Terrain,
                                   SceneFrameResourceStage::NetworkRequest,
                                   FrameResourcePriority::Normal));
    EXPECT_EQ(1u, arbiter.granted(SceneFrameResourceProducer::Raster,
                                  SceneFrameResourceStage::GpuUpload,
                                  FrameResourcePriority::Normal));
    EXPECT_TRUE(arbiter.tryAcquire(
        SceneFrameResourceProducer::Terrain,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal));
    EXPECT_FALSE(arbiter.tryAcquire(
        SceneFrameResourceProducer::Terrain,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal));
    EXPECT_TRUE(arbiter.tryAcquire(
        SceneFrameResourceProducer::Raster,
        SceneFrameResourceStage::GpuUpload,
        FrameResourcePriority::Normal));
}

TEST(SceneFrameResourceArbiterTest, PriorityIsAllocatedBeforeLowerClass) {
    SceneFrameResourceArbiter arbiter;
    arbiter.beginFrame(1, makeConfig(1));
    EXPECT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::PageStore,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Preload));
    EXPECT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Mvt,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal));
    EXPECT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Raster,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Urgent));
    EXPECT_TRUE(arbiter.sealAllocations());

    EXPECT_EQ(1u, arbiter.granted(SceneFrameResourceProducer::Raster,
                                  SceneFrameResourceStage::NetworkRequest,
                                  FrameResourcePriority::Urgent));
    EXPECT_EQ(0u, arbiter.granted(SceneFrameResourceProducer::Mvt,
                                  SceneFrameResourceStage::NetworkRequest,
                                  FrameResourcePriority::Normal));
    const auto snapshot = arbiter.snapshot();
    const auto& priorities =
        snapshot.stage(SceneFrameResourceStage::NetworkRequest).priorities;
    EXPECT_EQ(1u, priorities[static_cast<std::size_t>(
                       FrameResourcePriority::Urgent)]
                       .granted);
    EXPECT_EQ(1u, priorities[static_cast<std::size_t>(
                       FrameResourcePriority::Normal)]
                       .deferred);
}

TEST(SceneFrameResourceArbiterTest, LeaseRejectsStaleFrameAndRequiresSeal) {
    SceneFrameResourceArbiter arbiter;
    arbiter.beginFrame(1, makeConfig(2));
    auto terrain = arbiter.lease(SceneFrameResourceProducer::Terrain);
    EXPECT_TRUE(terrain.valid());
    EXPECT_TRUE(terrain.declareDemand(
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Urgent));
    EXPECT_FALSE(terrain.tryAcquire(
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Urgent));

    EXPECT_TRUE(arbiter.sealAllocations());
    EXPECT_FALSE(terrain.declareDemand(
        SceneFrameResourceStage::WorkerDispatch,
        FrameResourcePriority::Urgent));
    EXPECT_TRUE(terrain.tryAcquire(
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Urgent));

    // A repeated external frame number still starts a new lease generation.
    arbiter.beginFrame(1, makeConfig(2));
    EXPECT_FALSE(terrain.valid());
    EXPECT_FALSE(terrain.declareDemand(
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Urgent));
}

TEST(SceneFrameResourceArbiterTest, SnapshotReportsTotalsAndDeferredDemand) {
    SceneFrameResourceArbiterConfig config = makeConfig(2);
    SceneFrameResourceArbiter arbiter;
    arbiter.beginFrame(99, config);
    EXPECT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Terrain,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Urgent,
        2));
    EXPECT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::PageStore,
        SceneFrameResourceStage::ComposeDispatch,
        FrameResourcePriority::Normal,
        3));
    EXPECT_TRUE(arbiter.sealAllocations());
    EXPECT_TRUE(arbiter.tryAcquire(
        SceneFrameResourceProducer::Terrain,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Urgent));

    const auto snapshot = arbiter.snapshot();
    EXPECT_EQ(5u, snapshot.totals.demand);
    EXPECT_EQ(4u, snapshot.totals.granted);
    EXPECT_EQ(1u, snapshot.totals.used);
    EXPECT_EQ(1u, snapshot.totals.deferred);
    EXPECT_EQ(3u, snapshot.stage(SceneFrameResourceStage::ComposeDispatch)
                       .demand);
    EXPECT_EQ(1u, snapshot.stage(SceneFrameResourceStage::ComposeDispatch)
                       .deferred);
}

TEST(SceneFrameResourceArbiterTest, AllProducersShareOneStageBudget) {
    SceneFrameResourceArbiterConfig config = makeConfig(2);
    SceneFrameResourceArbiter arbiter;
    arbiter.beginFrame(7, config);
    for (const auto producer : {SceneFrameResourceProducer::Terrain,
                                SceneFrameResourceProducer::Raster,
                                SceneFrameResourceProducer::Mvt,
                                SceneFrameResourceProducer::PageStore}) {
        ASSERT_TRUE(arbiter.declareDemand(
            producer, SceneFrameResourceStage::NetworkRequest,
            FrameResourcePriority::Normal));
    }
    ASSERT_TRUE(arbiter.sealAllocations());

    const auto snapshot = arbiter.snapshot();
    EXPECT_EQ(4u, snapshot.stage(SceneFrameResourceStage::NetworkRequest)
                      .demand);
    EXPECT_EQ(2u, snapshot.stage(SceneFrameResourceStage::NetworkRequest)
                      .granted);
    EXPECT_EQ(2u, snapshot.stage(SceneFrameResourceStage::NetworkRequest)
                      .deferred);
    EXPECT_EQ(2u, snapshot.totals.granted);
    EXPECT_LE(snapshot.stage(SceneFrameResourceStage::NetworkRequest).granted,
              snapshot.stage(SceneFrameResourceStage::NetworkRequest).budget);
}

TEST(SceneFrameResourceArbiterTest, TerrainUrgentIsProtectedAcrossSystems) {
    SceneFrameResourceArbiterConfig config = makeConfig(2);
    config.networkRequest.reservedUrgentTerrainUnits = 1;
    SceneFrameResourceArbiter arbiter;
    arbiter.beginFrame(8, config);
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Terrain,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Urgent,
        2));
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Raster,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Urgent,
        1));
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Mvt,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal,
        4));
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::PageStore,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal,
        4));
    ASSERT_TRUE(arbiter.sealAllocations());

    EXPECT_EQ(1u, arbiter.granted(SceneFrameResourceProducer::Terrain,
                                   SceneFrameResourceStage::NetworkRequest,
                                   FrameResourcePriority::Urgent));
    EXPECT_EQ(1u, arbiter.granted(SceneFrameResourceProducer::Raster,
                                   SceneFrameResourceStage::NetworkRequest,
                                   FrameResourcePriority::Urgent));
    EXPECT_EQ(1u, arbiter.snapshot()
                      .stage(SceneFrameResourceStage::NetworkRequest)
                      .reservedUrgentTerrainGranted);
    EXPECT_EQ(2u, arbiter.snapshot()
                      .stage(SceneFrameResourceStage::NetworkRequest)
                      .granted);
}

TEST(SceneFrameResourceArbiterTest, StageAccountingIsIndependentAcrossProducers) {
    SceneFrameResourceArbiterConfig config = makeConfig(1, 1, 1, 1, 1);
    SceneFrameResourceArbiter arbiter;
    arbiter.beginFrame(9, config);
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Terrain,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal,
        3));
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Mvt,
        SceneFrameResourceStage::WorkerDispatch,
        FrameResourcePriority::Normal,
        3));
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::PageStore,
        SceneFrameResourceStage::GpuUpload,
        FrameResourcePriority::Normal,
        3));
    ASSERT_TRUE(arbiter.sealAllocations());

    EXPECT_EQ(1u, arbiter.granted(SceneFrameResourceProducer::Terrain,
                                   SceneFrameResourceStage::NetworkRequest,
                                   FrameResourcePriority::Normal));
    EXPECT_EQ(1u, arbiter.granted(SceneFrameResourceProducer::Mvt,
                                  SceneFrameResourceStage::WorkerDispatch,
                                  FrameResourcePriority::Normal));
    EXPECT_EQ(1u, arbiter.granted(SceneFrameResourceProducer::PageStore,
                                  SceneFrameResourceStage::GpuUpload,
                                  FrameResourcePriority::Normal));
    EXPECT_EQ(3u, arbiter.snapshot().totals.granted);
}

TEST(SceneFrameResourceArbiterTest,
     SuggestedDemandTracksUsedAndAdmissionDeniedWork) {
    SceneFrameResourceArbiter arbiter;
    SceneFrameResourceArbiterConfig config = makeConfig(1);
    arbiter.beginFrame(1, config);
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Mvt,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal));
    ASSERT_TRUE(arbiter.sealAllocations());
    EXPECT_TRUE(arbiter.tryAcquire(
        SceneFrameResourceProducer::Mvt,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal));
    EXPECT_FALSE(arbiter.tryAcquire(
        SceneFrameResourceProducer::Mvt,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal));
    EXPECT_EQ(1u, arbiter.snapshot()
                      .producerStage(
                          SceneFrameResourceProducer::Mvt,
                          SceneFrameResourceStage::NetworkRequest)
                      .deferred);

    arbiter.beginFrame(2, config);
    EXPECT_EQ(2u, arbiter.suggestedDemand(
                      SceneFrameResourceProducer::Mvt,
                      SceneFrameResourceStage::NetworkRequest,
                      FrameResourcePriority::Normal));
}

TEST(SceneFrameResourceArbiterTest,
     SuggestedDemandDropsUnusedFixedDeclaration) {
    SceneFrameResourceArbiter arbiter;
    SceneFrameResourceArbiterConfig config = makeConfig(8);
    arbiter.beginFrame(1, config);
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::PageStore,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal,
        8));
    ASSERT_TRUE(arbiter.sealAllocations());

    arbiter.beginFrame(2, config);
    EXPECT_EQ(0u, arbiter.suggestedDemand(
                      SceneFrameResourceProducer::PageStore,
                      SceneFrameResourceStage::NetworkRequest,
                      FrameResourcePriority::Normal));
}

TEST(SceneFrameResourceArbiterTest,
     InstanceDeferredDemandExpandsNextFrameWithoutGlobalDenial) {
    SceneFrameResourceArbiter arbiter;
    SceneFrameResourceArbiterConfig config = makeConfig(4);
    arbiter.beginFrame(1, config);
    ASSERT_TRUE(arbiter.declareDemand(
        SceneFrameResourceProducer::Mvt,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal,
        4));
    ASSERT_TRUE(arbiter.sealAllocations());
    ASSERT_TRUE(arbiter.tryAcquire(
        SceneFrameResourceProducer::Mvt,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal));
    arbiter.noteInstanceDeferredDemand(
        SceneFrameResourceProducer::Mvt,
        SceneFrameResourceStage::NetworkRequest,
        FrameResourcePriority::Normal,
        3);
    EXPECT_EQ(3u, arbiter.snapshot()
                      .producerStage(
                          SceneFrameResourceProducer::Mvt,
                          SceneFrameResourceStage::NetworkRequest)
                      .deferred);

    arbiter.beginFrame(2, config);
    EXPECT_EQ(4u, arbiter.suggestedDemand(
                      SceneFrameResourceProducer::Mvt,
                      SceneFrameResourceStage::NetworkRequest,
                      FrameResourcePriority::Normal));
}
