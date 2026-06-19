#include <gtest/gtest.h>

#include "earth_engine/renderer/RenderCommandStreamingSet.h"

using namespace earth_engine;

namespace {

RenderCommand surfaceCommand(const std::string& key,
                             uint64_t frameId,
                             int indexCount) {
    RenderCommand command;
    command.kind = RenderCommandKind::SurfaceTile;
    command.owner = "surface_tile";
    command.pass = "color";
    command.stableKey = key;
    command.frameId = frameId;
    command.generation = frameId;
    command.hasSurfaceTileUniforms = true;
    command.vertexBuffer = reinterpret_cast<Buffer*>(0x1);
    command.indexCount = indexCount;
    command.indexType = RenderCommand::IndexType::UInt32;
    command.vertexStride = 32;
    command.surfaceMeshIndexCount = indexCount;
    return command;
}

} // namespace

TEST(RenderCommandStreamingSetTest, MutatesStableCommandsInPlaceByKey) {
    RenderCommandStreamingSet set;

    RenderCommandList frame1Candidates{
        surfaceCommand("terrain/0/0/0#0", 1, 6),
        surfaceCommand("terrain/1/0/0#0", 1, 12)};
    RenderCommandList frame1Active;
    set.update(frame1Candidates, 1, frame1Active);

    ASSERT_EQ(2u, frame1Active.size());
    EXPECT_EQ(2u, set.storedCommandCount());
    EXPECT_EQ("terrain/0/0/0#0", frame1Active[0].stableKey);
    EXPECT_EQ(6, frame1Active[0].indexCount);

    RenderCommandList frame2Candidates{
        surfaceCommand("terrain/0/0/0#0", 2, 18),
        surfaceCommand("terrain/1/1/0#0", 2, 24)};
    RenderCommandList frame2Active;
    set.update(frame2Candidates, 2, frame2Active);

    ASSERT_EQ(2u, frame2Active.size());
    EXPECT_EQ(2u, set.storedCommandCount());
    EXPECT_EQ("terrain/0/0/0#0", frame2Active[0].stableKey);
    EXPECT_EQ(18, frame2Active[0].indexCount);
    EXPECT_EQ("terrain/1/1/0#0", frame2Active[1].stableKey);
    EXPECT_EQ(24, frame2Active[1].indexCount);
}

TEST(RenderCommandStreamingSetTest, LeavesTransientCommandsInFrameListOnly) {
    RenderCommandStreamingSet set;

    RenderCommand transient;
    transient.kind = RenderCommandKind::VectorOverlay;
    transient.owner = "vector";
    transient.pass = "color";

    RenderCommandList candidates{
        transient,
        surfaceCommand("terrain/0/0/0#0", 3, 6)};
    RenderCommandList active;
    set.update(candidates, 3, active);

    ASSERT_EQ(2u, active.size());
    EXPECT_TRUE(active[0].stableKey.empty());
    EXPECT_EQ(RenderCommandKind::VectorOverlay, active[0].kind);
    EXPECT_EQ("terrain/0/0/0#0", active[1].stableKey);
    EXPECT_EQ(1u, set.storedCommandCount());
}

TEST(RenderCommandStreamingSetTest, DeduplicatesAccidentalDuplicateKeys) {
    RenderCommandStreamingSet set;

    RenderCommandList candidates{
        surfaceCommand("terrain/0/0/0#0", 4, 6),
        surfaceCommand("terrain/0/0/0#0", 4, 18)};
    RenderCommandList active;
    set.update(candidates, 4, active);

    ASSERT_EQ(1u, active.size());
    EXPECT_EQ("terrain/0/0/0#0", active[0].stableKey);
    EXPECT_EQ(18, active[0].indexCount);
    EXPECT_EQ(1u, set.storedCommandCount());
}
