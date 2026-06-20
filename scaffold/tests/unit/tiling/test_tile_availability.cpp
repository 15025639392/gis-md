#include <gtest/gtest.h>

#include "earth_engine/tiling/TileAvailability.h"

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

using namespace earth_engine;

TEST(TileAvailabilityUtilitiesTest, CountOnesInByteMatchesCesiumNative) {
    for (uint16_t value = 0; value <= 0xFFU; ++value) {
        uint8_t expected = 0;
        uint8_t bits = static_cast<uint8_t>(value);
        while (bits != 0) {
            expected += static_cast<uint8_t>(bits & 1U);
            bits >>= 1U;
        }

        EXPECT_EQ(expected,
                  TileAvailabilityUtilities::countOnesInByte(
                      static_cast<uint8_t>(value)));
    }
}

TEST(TileAvailabilityUtilitiesTest, CountOnesInBufferMatchesCesiumNative) {
    std::vector<std::byte> buffer(64);
    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = static_cast<std::byte>(0xFC);
    }

    EXPECT_EQ(384U, TileAvailabilityUtilities::countOnesInBuffer(buffer));
}

TEST(TileAvailabilityAccessorTest, ConstantAvailabilityMatchesCesiumNative) {
    TileAvailabilitySubtree subtree{
        ConstantTileAvailability{true},
        TileSubtreeBufferView{0, 64, 0},
        ConstantTileAvailability{false},
        {std::vector<std::byte>(64, static_cast<std::byte>(0xFC))}};

    TileAvailabilityAccessor tileAccessor(
        subtree.tileAvailability,
        subtree);
    TileAvailabilityAccessor subtreeAccessor(
        subtree.subtreeAvailability,
        subtree);

    EXPECT_TRUE(tileAccessor.isConstant());
    EXPECT_TRUE(tileAccessor.getConstant());
    EXPECT_FALSE(tileAccessor.isBufferView());
    EXPECT_TRUE(subtreeAccessor.isConstant());
    EXPECT_FALSE(subtreeAccessor.getConstant());
    EXPECT_FALSE(subtreeAccessor.isBufferView());
}

TEST(TileAvailabilityAccessorTest, BufferAvailabilityMatchesCesiumNative) {
    TileAvailabilitySubtree subtree{
        ConstantTileAvailability{true},
        TileSubtreeBufferView{0, 64, 0},
        ConstantTileAvailability{false},
        {std::vector<std::byte>(64, static_cast<std::byte>(0xFC))}};

    TileAvailabilityAccessor contentAccessor(
        subtree.contentAvailability,
        subtree);

    ASSERT_FALSE(contentAccessor.isConstant());
    ASSERT_TRUE(contentAccessor.isBufferView());
    EXPECT_EQ(64U, contentAccessor.size());
    for (size_t i = 0; i < contentAccessor.size(); ++i) {
        EXPECT_EQ(static_cast<std::byte>(0xFC), contentAccessor[i]);
    }
}

TEST(TileAvailabilityAccessorTest, CombinedBufferAvailabilityMatchesCesiumNative) {
    std::vector<std::byte> availabilityBuffer(64);
    for (size_t i = 0; i < availabilityBuffer.size(); ++i) {
        availabilityBuffer[i] = static_cast<std::byte>(0xFC);
    }

    TileAvailabilitySubtree subtree{
        TileSubtreeBufferView{0, 32, 0},
        TileSubtreeBufferView{32, 32, 0},
        ConstantTileAvailability{false},
        {std::move(availabilityBuffer)}};

    TileAvailabilityAccessor tileAccessor(
        subtree.tileAvailability,
        subtree);
    TileAvailabilityAccessor contentAccessor(
        subtree.contentAvailability,
        subtree);

    ASSERT_TRUE(tileAccessor.isBufferView());
    ASSERT_TRUE(contentAccessor.isBufferView());
    EXPECT_EQ(32U, tileAccessor.size());
    EXPECT_EQ(32U, contentAccessor.size());
    for (size_t i = 0; i < 32U; ++i) {
        EXPECT_EQ(static_cast<std::byte>(0xFC), tileAccessor[i]);
        EXPECT_EQ(static_cast<std::byte>(0xFC), contentAccessor[i]);
    }
}

TEST(TileAvailabilityAccessorTest, InvalidBufferViewIsNotReadableLikeCesiumNative) {
    TileAvailabilitySubtree subtree{
        TileSubtreeBufferView{0, 4, 1},
        TileSubtreeBufferView{6, 4, 0},
        ConstantTileAvailability{false},
        {std::vector<std::byte>(8, static_cast<std::byte>(0xFF))}};

    TileAvailabilityAccessor missingBufferAccessor(
        subtree.tileAvailability,
        subtree);
    TileAvailabilityAccessor outOfRangeAccessor(
        subtree.contentAvailability,
        subtree);

    EXPECT_FALSE(missingBufferAccessor.isConstant());
    EXPECT_FALSE(missingBufferAccessor.isBufferView());
    EXPECT_FALSE(outOfRangeAccessor.isConstant());
    EXPECT_FALSE(outOfRangeAccessor.isBufferView());
}

TEST(TileAvailabilityAccessorTest, OverflowingBufferViewRangeIsInvalidLikeCesiumNative) {
    TileAvailabilitySubtree subtree{
        TileSubtreeBufferView{UINT32_MAX - 1U, 4, 0},
        ConstantTileAvailability{true},
        ConstantTileAvailability{false},
        {std::vector<std::byte>(8, static_cast<std::byte>(0xFF))}};

    TileAvailabilityAccessor accessor(subtree.tileAvailability, subtree);

    EXPECT_FALSE(accessor.isConstant());
    EXPECT_FALSE(accessor.isBufferView());
    EXPECT_EQ(0U, accessor.size());
    EXPECT_EQ(nullptr, accessor.getBufferAccessor());
}

TEST(TileAvailabilityAccessorTest, EmptyBufferViewRangeIsReadableLikeCesiumNative) {
    TileAvailabilitySubtree subtree{
        TileSubtreeBufferView{0, 0, 0},
        ConstantTileAvailability{true},
        ConstantTileAvailability{false},
        {std::vector<std::byte>{}}};

    TileAvailabilityAccessor accessor(subtree.tileAvailability, subtree);

    EXPECT_FALSE(accessor.isConstant());
    EXPECT_TRUE(accessor.isBufferView());
    EXPECT_EQ(0U, accessor.size());
}

TEST(TileAvailabilityNodeTest, LoadedSubtreeChildCountUsesSubtreeAvailability) {
    TileAvailabilityNode constantNode;
    constantNode.setLoadedSubtree(
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            {}},
        64);
    EXPECT_EQ(64U, constantNode.childNodes.size());

    TileAvailabilityNode bufferNode;
    bufferNode.setLoadedSubtree(
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            TileSubtreeBufferView{0, 8, 0},
            {std::vector<std::byte>{
                static_cast<std::byte>(0xFF),
                static_cast<std::byte>(0xFF),
                static_cast<std::byte>(0xFF),
                static_cast<std::byte>(0xFF),
                static_cast<std::byte>(0xFF),
                static_cast<std::byte>(0x0F),
                static_cast<std::byte>(0xFF),
                static_cast<std::byte>(0xFF)}}},
        64);
    EXPECT_EQ(60U, bufferNode.childNodes.size());
}

namespace {

bool containsTile(const std::vector<std::array<uint32_t, 3>>& ids,
                  uint32_t level,
                  uint32_t x,
                  uint32_t y) {
    for (const auto& id : ids) {
        if (id[0] == level && id[1] == x && id[2] == y) {
            return true;
        }
    }
    return false;
}

bool containsOctreeTile(const std::vector<OctreeTileID>& ids,
                        uint32_t level,
                        uint32_t x,
                        uint32_t y,
                        uint32_t z) {
    for (const OctreeTileID& id : ids) {
        if (id.level == static_cast<int>(level) &&
            id.x == static_cast<int>(x) &&
            id.y == static_cast<int>(y) &&
            id.z == static_cast<int>(z)) {
            return true;
        }
    }
    return false;
}

TileAvailabilitySubtree makeQuadtreeFixtureRootSubtree() {
    std::vector<std::byte> contentAvailabilityBuffer = {
        static_cast<std::byte>(0xFF),
        static_cast<std::byte>(0x0F),
        static_cast<std::byte>(0x3F),
        static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00)};

    std::vector<std::byte> subtreeAvailabilityBuffer = {
        static_cast<std::byte>(0xFF),
        static_cast<std::byte>(0xFF),
        static_cast<std::byte>(0xFF),
        static_cast<std::byte>(0xFF),
        static_cast<std::byte>(0xFF),
        static_cast<std::byte>(0x0F),
        static_cast<std::byte>(0xFF),
        static_cast<std::byte>(0xFF)};

    return TileAvailabilitySubtree{
        ConstantTileAvailability{true},
        TileSubtreeBufferView{0, 8, 0},
        TileSubtreeBufferView{0, 8, 1},
        {std::move(contentAvailabilityBuffer),
         std::move(subtreeAvailabilityBuffer)}};
}

TileAvailabilitySubtree makeOctreeFixtureRootSubtree() {
    std::vector<std::byte> contentAvailabilityBuffer(16);
    for (size_t i = 0; i < 9U; ++i) {
        contentAvailabilityBuffer[i] = static_cast<std::byte>(0xFF);
    }
    contentAvailabilityBuffer[9] = static_cast<std::byte>(0x01);
    contentAvailabilityBuffer[1] = static_cast<std::byte>(0x0F);

    std::vector<std::byte> subtreeAvailabilityBuffer(64);
    for (size_t i = 0; i < subtreeAvailabilityBuffer.size(); ++i) {
        subtreeAvailabilityBuffer[i] = static_cast<std::byte>(0xFF);
    }
    subtreeAvailabilityBuffer[5] = static_cast<std::byte>(0x0F);

    return TileAvailabilitySubtree{
        ConstantTileAvailability{true},
        TileSubtreeBufferView{0, 16, 0},
        TileSubtreeBufferView{0, 64, 1},
        {std::move(contentAvailabilityBuffer),
         std::move(subtreeAvailabilityBuffer)}};
}

} // namespace

TEST(TileQuadtreeAvailabilityTest, RootIsImplicitlyAvailableBeforeSubtreeLoads) {
    TileQuadtreeAvailability availability(3, 5);

    EXPECT_EQ(3U, availability.subtreeLevels());
    EXPECT_EQ(5U, availability.maximumLevel());
    EXPECT_EQ(static_cast<uint8_t>(TileAvailable | SubtreeAvailable),
              availability.computeAvailability(0, 0, 0));
    EXPECT_EQ(0, availability.computeAvailability(1, 0, 0));
}

TEST(TileQuadtreeAvailabilityTest, UnloadedNodeOnlyMakesSubtreeRootAvailable) {
    TileQuadtreeAvailability availability(3, 5);

    EXPECT_EQ(static_cast<uint8_t>(TileAvailable | SubtreeAvailable),
              availability.computeAvailability(0, 0, 0, nullptr));
    EXPECT_EQ(0, availability.computeAvailability(1, 0, 0, nullptr));
    EXPECT_EQ(0, availability.computeAvailability(2, 0, 0, nullptr));
    EXPECT_EQ(static_cast<uint8_t>(TileAvailable | SubtreeAvailable),
              availability.computeAvailability(3, 0, 0, nullptr));
}

TEST(TileQuadtreeAvailabilityTest, ZeroSubtreeLevelsAreUnavailable) {
    TileQuadtreeAvailability availability(0, 5);
    TileQuadtreeAvailability overflowingAvailability(16, 20);

    EXPECT_EQ(0U, availability.subtreeLevels());
    EXPECT_EQ(0, availability.computeAvailability(0, 0, 0));
    EXPECT_FALSE(availability.addSubtree(
        0,
        0,
        0,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            {}}));
    EXPECT_EQ(nullptr, availability.addNode(0, 0, 0, nullptr));
    EXPECT_EQ(nullptr, availability.rootNode());

    EXPECT_EQ(0, overflowingAvailability.computeAvailability(0, 0, 0));
    EXPECT_FALSE(overflowingAvailability.addSubtree(
        0,
        0,
        0,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            {}}));
}

TEST(TileQuadtreeAvailabilityTest, TileAndContentAvailabilityMatchCesiumNative) {
    TileQuadtreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(0, 0, 0, makeQuadtreeFixtureRootSubtree()));
    const TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);

    const std::vector<std::array<uint32_t, 3>> unavailableContentIds = {
        std::array<uint32_t, 3>{2, 3, 1},
        std::array<uint32_t, 3>{2, 0, 2},
        std::array<uint32_t, 3>{2, 1, 2},
        std::array<uint32_t, 3>{2, 0, 3}};

    for (uint32_t level = 0; level < 3U; ++level) {
        for (uint32_t y = 0; y < (1U << level); ++y) {
            for (uint32_t x = 0; x < (1U << level); ++x) {
                const uint8_t global =
                    availability.computeAvailability(level, x, y);
                const uint8_t primed =
                    availability.computeAvailability(level, x, y, root);
                EXPECT_EQ(global, primed);
                EXPECT_TRUE((global & TileAvailable) != 0);
                EXPECT_EQ(
                    !containsTile(unavailableContentIds, level, x, y),
                    (global & ContentAvailable) != 0);
            }
        }
    }
}

TEST(TileQuadtreeAvailabilityTest, OutOfRangeCoordinatesAreUnavailableLikeCesiumNative) {
    TileQuadtreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(0, 0, 0, makeQuadtreeFixtureRootSubtree()));
    const TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);

    EXPECT_EQ(0, availability.computeAvailability(0, 1, 1));
    EXPECT_EQ(0, availability.computeAvailability(1, 2, 0));
    EXPECT_EQ(0, availability.computeAvailability(1, 0, 2));
    EXPECT_EQ(0, availability.computeAvailability(2, 4, 1));
    EXPECT_EQ(0, availability.computeAvailability(2, 1, 4));

    EXPECT_EQ(0, availability.computeAvailability(0, 1, 1, root));
    EXPECT_EQ(0, availability.computeAvailability(1, 2, 0, root));
    EXPECT_EQ(0, availability.computeAvailability(1, 0, 2, root));
    EXPECT_EQ(0, availability.computeAvailability(2, 4, 1, root));
    EXPECT_EQ(0, availability.computeAvailability(2, 1, 4, root));
}

TEST(TileQuadtreeAvailabilityTest, ChildSubtreeAvailabilityMatchesCesiumNative) {
    TileQuadtreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(0, 0, 0, makeQuadtreeFixtureRootSubtree()));
    TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);

    const std::vector<std::array<uint32_t, 3>> unavailableSubtreeIds = {
        std::array<uint32_t, 3>{3, 2, 6},
        std::array<uint32_t, 3>{3, 3, 6},
        std::array<uint32_t, 3>{3, 2, 7},
        std::array<uint32_t, 3>{3, 3, 7}};

    for (uint32_t y = 0; y < 8U; ++y) {
        for (uint32_t x = 0; x < 8U; ++x) {
            const uint8_t state = availability.computeAvailability(3, x, y);
            const std::optional<uint32_t> childIndex =
                availability.findChildNodeIndex(3, x, y, root);
            const bool subtreeShouldBeAvailable =
                !containsTile(unavailableSubtreeIds, 3, x, y);

            EXPECT_EQ(subtreeShouldBeAvailable,
                      (state & SubtreeAvailable) != 0);
            EXPECT_EQ(subtreeShouldBeAvailable, childIndex.has_value());
        }
    }
}

TEST(TileQuadtreeAvailabilityTest, ChildSubtreeLoadedFlagMatchesCesiumNative) {
    TileQuadtreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(0, 0, 0, makeQuadtreeFixtureRootSubtree()));
    TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);

    const std::vector<std::array<uint32_t, 3>> loadedSubtreeIds = {
        std::array<uint32_t, 3>{3, 0, 0},
        std::array<uint32_t, 3>{3, 0, 1},
        std::array<uint32_t, 3>{3, 0, 2},
        std::array<uint32_t, 3>{3, 1, 2}};

    for (const auto& id : loadedSubtreeIds) {
        ASSERT_TRUE(availability.addSubtree(
            id[0],
            id[1],
            id[2],
            TileAvailabilitySubtree{
                ConstantTileAvailability{true},
                ConstantTileAvailability{true},
                ConstantTileAvailability{false},
                {}}));
    }

    for (uint32_t y = 0; y < 8U; ++y) {
        for (uint32_t x = 0; x < 8U; ++x) {
            const uint8_t state = availability.computeAvailability(3, x, y);
            const TileAvailabilityNode* child =
                availability.findChildNode(3, x, y, root);
            const bool subtreeShouldBeLoaded =
                containsTile(loadedSubtreeIds, 3, x, y);

            EXPECT_EQ(subtreeShouldBeLoaded, (state & SubtreeLoaded) != 0);
            EXPECT_EQ(subtreeShouldBeLoaded, child != nullptr);
        }
    }
}

TEST(TileQuadtreeAvailabilityTest, AddSubtreeTraversesLoadedDescendantNodes) {
    TileQuadtreeAvailability availability(3, 8);
    ASSERT_TRUE(availability.addSubtree(0, 0, 0, makeQuadtreeFixtureRootSubtree()));
    ASSERT_TRUE(availability.addSubtree(
        3,
        0,
        0,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            {}}));

    ASSERT_TRUE(availability.addSubtree(
        6,
        0,
        0,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));

    const uint8_t loadedState = availability.computeAvailability(6, 0, 0);
    EXPECT_TRUE((loadedState & TileAvailable) != 0);
    EXPECT_TRUE((loadedState & SubtreeAvailable) != 0);
    EXPECT_TRUE((loadedState & SubtreeLoaded) != 0);

    EXPECT_FALSE(availability.addSubtree(
        6,
        0,
        0,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));
}

TEST(TileQuadtreeAvailabilityTest, AddSubtreeRejectsNonBoundaryOrUnavailableChildLikeCesiumNative) {
    TileQuadtreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(0, 0, 0, makeQuadtreeFixtureRootSubtree()));

    EXPECT_FALSE(availability.addSubtree(
        2,
        0,
        0,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));

    EXPECT_FALSE(availability.addSubtree(
        3,
        2,
        6,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));
}

TEST(TileQuadtreeAvailabilityTest, AddSubtreeRejectsOutOfRangeChildCoordinates) {
    TileQuadtreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(
        0,
        0,
        0,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            {}}));
    TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);

    EXPECT_FALSE(availability.addSubtree(
        3,
        8,
        0,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));
    EXPECT_FALSE(availability.addSubtree(
        3,
        0,
        8,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));
    EXPECT_EQ(nullptr, root->childNodes[0].get());
}

TEST(TileQuadtreeAvailabilityTest, AddNodeAndAddLoadedSubtreeMatchCesiumNative) {
    TileQuadtreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(0, 0, 0, makeQuadtreeFixtureRootSubtree()));
    TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);

    TileAvailabilityNode* node = availability.addNode(3, 0, 1, root);
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(static_cast<uint8_t>(TileAvailable | SubtreeAvailable),
              availability.computeAvailability(3, 0, 1, node));

    EXPECT_TRUE(availability.addLoadedSubtree(
        node,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));
    EXPECT_FALSE(availability.addLoadedSubtree(
        node,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));

    const uint8_t loadedState = availability.computeAvailability(3, 0, 1);
    EXPECT_TRUE((loadedState & TileAvailable) != 0);
    EXPECT_TRUE((loadedState & SubtreeAvailable) != 0);
    EXPECT_TRUE((loadedState & SubtreeLoaded) != 0);
}

TEST(TileQuadtreeAvailabilityTest, AddNodeReplacesExistingChildNodeLikeCesiumNative) {
    TileQuadtreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(0, 0, 0, makeQuadtreeFixtureRootSubtree()));
    TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);

    TileAvailabilityNode* firstNode = availability.addNode(3, 0, 1, root);
    ASSERT_NE(nullptr, firstNode);
    ASSERT_TRUE(availability.addLoadedSubtree(
        firstNode,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));

    TileAvailabilityNode* secondNode = availability.addNode(3, 0, 1, root);
    ASSERT_NE(nullptr, secondNode);
    EXPECT_NE(firstNode, secondNode);
    EXPECT_EQ(secondNode, availability.findChildNode(3, 0, 1, root));

    const uint8_t replacedState = availability.computeAvailability(3, 0, 1);
    EXPECT_TRUE((replacedState & TileAvailable) != 0);
    EXPECT_TRUE((replacedState & SubtreeAvailable) != 0);
    EXPECT_FALSE((replacedState & SubtreeLoaded) != 0);
}

TEST(TileQuadtreeAvailabilityTest, AddSubtreeRejectsExistingEmptyNodeLikeCesiumNative) {
    TileQuadtreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(0, 0, 0, makeQuadtreeFixtureRootSubtree()));
    TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);
    ASSERT_NE(nullptr, availability.addNode(3, 0, 1, root));

    EXPECT_FALSE(availability.addSubtree(
        3,
        0,
        1,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));
}

TEST(TileQuadtreeAvailabilityTest, AddNodeRejectsNonBoundaryOrUnavailableChildLikeCesiumNative) {
    TileQuadtreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(0, 0, 0, makeQuadtreeFixtureRootSubtree()));
    TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);

    EXPECT_EQ(nullptr, availability.addNode(2, 0, 0, root));
    EXPECT_EQ(nullptr, availability.addNode(3, 2, 6, root));
}

TEST(TileQuadtreeAvailabilityTest, AddNodeRejectsOutOfRangeRootCoordinates) {
    TileQuadtreeAvailability availability(3, 5);

    EXPECT_EQ(nullptr, availability.addNode(0, 1, 0, nullptr));
    EXPECT_EQ(nullptr, availability.addNode(0, 0, 1, nullptr));
    EXPECT_EQ(nullptr, availability.rootNode());

    TileAvailabilityNode* root = availability.addNode(0, 0, 0, nullptr);
    EXPECT_NE(nullptr, root);
    EXPECT_EQ(root, availability.rootNode());
}

TEST(TileQuadtreeAvailabilityTest, AddNodeRejectsOutOfRangeChildCoordinates) {
    TileQuadtreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(
        0,
        0,
        0,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            {}}));
    TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);

    EXPECT_EQ(std::nullopt, availability.findChildNodeIndex(3, 8, 0, root));
    EXPECT_EQ(std::nullopt, availability.findChildNodeIndex(3, 0, 8, root));
    EXPECT_EQ(nullptr, availability.addNode(3, 8, 0, root));
    EXPECT_EQ(nullptr, availability.addNode(3, 0, 8, root));
    EXPECT_EQ(64U, root->childNodes.size());
}

TEST(TileOctreeAvailabilityTest, RootIsImplicitlyAvailableBeforeSubtreeLoads) {
    TileOctreeAvailability availability(3, 5);

    EXPECT_EQ(3U, availability.subtreeLevels());
    EXPECT_EQ(5U, availability.maximumLevel());
    EXPECT_EQ(static_cast<uint8_t>(TileAvailable | SubtreeAvailable),
              availability.computeAvailability(OctreeTileID{0, 0, 0, 0}));
    EXPECT_EQ(0, availability.computeAvailability(OctreeTileID{1, 0, 0, 0}));
}

TEST(TileOctreeAvailabilityTest, UnloadedNodeOnlyMakesSubtreeRootAvailable) {
    TileOctreeAvailability availability(3, 5);

    EXPECT_EQ(static_cast<uint8_t>(TileAvailable | SubtreeAvailable),
              availability.computeAvailability(
                  OctreeTileID{0, 0, 0, 0},
                  nullptr));
    EXPECT_EQ(0,
              availability.computeAvailability(
                  OctreeTileID{1, 0, 0, 0},
                  nullptr));
    EXPECT_EQ(0,
              availability.computeAvailability(
                  OctreeTileID{2, 0, 0, 0},
                  nullptr));
    EXPECT_EQ(static_cast<uint8_t>(TileAvailable | SubtreeAvailable),
              availability.computeAvailability(
                  OctreeTileID{3, 0, 0, 0},
                  nullptr));
}

TEST(TileOctreeAvailabilityTest, ZeroSubtreeLevelsAreUnavailable) {
    TileOctreeAvailability availability(0, 5);
    TileOctreeAvailability overflowingAvailability(11, 20);

    EXPECT_EQ(0U, availability.subtreeLevels());
    EXPECT_EQ(0, availability.computeAvailability(OctreeTileID{0, 0, 0, 0}));
    EXPECT_FALSE(availability.addSubtree(
        OctreeTileID{0, 0, 0, 0},
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            {}}));
    EXPECT_EQ(nullptr,
              availability.addNode(OctreeTileID{0, 0, 0, 0}, nullptr));
    EXPECT_EQ(nullptr, availability.rootNode());

    EXPECT_EQ(0,
              overflowingAvailability.computeAvailability(
                  OctreeTileID{0, 0, 0, 0}));
    EXPECT_FALSE(overflowingAvailability.addSubtree(
        OctreeTileID{0, 0, 0, 0},
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            {}}));
}

TEST(TileOctreeAvailabilityTest, TileAndContentAvailabilityMatchCesiumNative) {
    TileOctreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(
        OctreeTileID{0, 0, 0, 0},
        makeOctreeFixtureRootSubtree()));
    const TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);

    const std::vector<OctreeTileID> unavailableContentIds = {
        OctreeTileID{2, 1, 1, 0},
        OctreeTileID{2, 0, 0, 1},
        OctreeTileID{2, 1, 0, 1},
        OctreeTileID{2, 0, 1, 1}};

    for (uint32_t level = 0; level < 3U; ++level) {
        for (uint32_t z = 0; z < (1U << level); ++z) {
            for (uint32_t y = 0; y < (1U << level); ++y) {
                for (uint32_t x = 0; x < (1U << level); ++x) {
                    const OctreeTileID id{
                        static_cast<int>(level),
                        static_cast<int>(x),
                        static_cast<int>(y),
                        static_cast<int>(z)};
                    const uint8_t global =
                        availability.computeAvailability(id);
                    const uint8_t primed =
                        availability.computeAvailability(id, root);
                    EXPECT_EQ(global, primed);
                    EXPECT_TRUE((global & TileAvailable) != 0);
                    EXPECT_EQ(
                        !containsOctreeTile(
                            unavailableContentIds,
                            level,
                            x,
                            y,
                            z),
                        (global & ContentAvailable) != 0);
                }
            }
        }
    }
}

TEST(TileOctreeAvailabilityTest, OutOfRangeCoordinatesAreUnavailableLikeCesiumNative) {
    TileOctreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(
        OctreeTileID{0, 0, 0, 0},
        makeOctreeFixtureRootSubtree()));
    const TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);

    EXPECT_EQ(0, availability.computeAvailability(OctreeTileID{0, 1, 1, 1}));
    EXPECT_EQ(0, availability.computeAvailability(OctreeTileID{1, 2, 0, 0}));
    EXPECT_EQ(0, availability.computeAvailability(OctreeTileID{1, 0, 2, 0}));
    EXPECT_EQ(0, availability.computeAvailability(OctreeTileID{1, 0, 0, 2}));
    EXPECT_EQ(0, availability.computeAvailability(OctreeTileID{2, 4, 1, 1}));
    EXPECT_EQ(0, availability.computeAvailability(OctreeTileID{2, 1, 4, 1}));
    EXPECT_EQ(0, availability.computeAvailability(OctreeTileID{2, 1, 1, 4}));

    EXPECT_EQ(0, availability.computeAvailability(OctreeTileID{0, 1, 1, 1}, root));
    EXPECT_EQ(0, availability.computeAvailability(OctreeTileID{1, 2, 0, 0}, root));
    EXPECT_EQ(0, availability.computeAvailability(OctreeTileID{1, 0, 2, 0}, root));
    EXPECT_EQ(0, availability.computeAvailability(OctreeTileID{1, 0, 0, 2}, root));
    EXPECT_EQ(0, availability.computeAvailability(OctreeTileID{2, 4, 1, 1}, root));
    EXPECT_EQ(0, availability.computeAvailability(OctreeTileID{2, 1, 4, 1}, root));
    EXPECT_EQ(0, availability.computeAvailability(OctreeTileID{2, 1, 1, 4}, root));
}

TEST(TileOctreeAvailabilityTest, ChildSubtreeAvailabilityMatchesCesiumNative) {
    TileOctreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(
        OctreeTileID{0, 0, 0, 0},
        makeOctreeFixtureRootSubtree()));
    TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);

    const std::vector<OctreeTileID> unavailableSubtreeIds = {
        OctreeTileID{3, 2, 0, 3},
        OctreeTileID{3, 3, 0, 3},
        OctreeTileID{3, 2, 1, 3},
        OctreeTileID{3, 3, 1, 3}};

    for (uint32_t z = 0; z < 8U; ++z) {
        for (uint32_t y = 0; y < 8U; ++y) {
            for (uint32_t x = 0; x < 8U; ++x) {
                const OctreeTileID id{
                    3,
                    static_cast<int>(x),
                    static_cast<int>(y),
                    static_cast<int>(z)};
                const uint8_t state = availability.computeAvailability(id);
                const std::optional<uint32_t> childIndex =
                    availability.findChildNodeIndex(id, root);
                const bool subtreeShouldBeAvailable =
                    !containsOctreeTile(unavailableSubtreeIds, 3, x, y, z);

                EXPECT_EQ(subtreeShouldBeAvailable,
                          (state & SubtreeAvailable) != 0);
                EXPECT_EQ(subtreeShouldBeAvailable, childIndex.has_value());
            }
        }
    }
}

TEST(TileOctreeAvailabilityTest, ChildSubtreeLoadedFlagMatchesCesiumNative) {
    TileOctreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(
        OctreeTileID{0, 0, 0, 0},
        makeOctreeFixtureRootSubtree()));
    TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);

    const std::vector<OctreeTileID> loadedSubtreeIds = {
        OctreeTileID{3, 0, 0, 0},
        OctreeTileID{3, 0, 1, 0},
        OctreeTileID{3, 0, 2, 0},
        OctreeTileID{3, 1, 2, 1}};

    for (const OctreeTileID& id : loadedSubtreeIds) {
        ASSERT_TRUE(availability.addSubtree(
            id,
            TileAvailabilitySubtree{
                ConstantTileAvailability{true},
                ConstantTileAvailability{true},
                ConstantTileAvailability{false},
                {}}));
    }

    for (uint32_t z = 0; z < 8U; ++z) {
        for (uint32_t y = 0; y < 8U; ++y) {
            for (uint32_t x = 0; x < 8U; ++x) {
                const OctreeTileID id{
                    3,
                    static_cast<int>(x),
                    static_cast<int>(y),
                    static_cast<int>(z)};
                const uint8_t state = availability.computeAvailability(id);
                const TileAvailabilityNode* child =
                    availability.findChildNode(id, root);
                const bool subtreeShouldBeLoaded =
                    containsOctreeTile(loadedSubtreeIds, 3, x, y, z);

                EXPECT_EQ(subtreeShouldBeLoaded,
                          (state & SubtreeLoaded) != 0);
                EXPECT_EQ(subtreeShouldBeLoaded, child != nullptr);
            }
        }
    }
}

TEST(TileOctreeAvailabilityTest, AddSubtreeTraversesLoadedDescendantNodes) {
    TileOctreeAvailability availability(3, 8);
    ASSERT_TRUE(availability.addSubtree(
        OctreeTileID{0, 0, 0, 0},
        makeOctreeFixtureRootSubtree()));
    ASSERT_TRUE(availability.addSubtree(
        OctreeTileID{3, 0, 0, 0},
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            {}}));

    ASSERT_TRUE(availability.addSubtree(
        OctreeTileID{6, 0, 0, 0},
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));

    const uint8_t loadedState =
        availability.computeAvailability(OctreeTileID{6, 0, 0, 0});
    EXPECT_TRUE((loadedState & TileAvailable) != 0);
    EXPECT_TRUE((loadedState & SubtreeAvailable) != 0);
    EXPECT_TRUE((loadedState & SubtreeLoaded) != 0);

    EXPECT_FALSE(availability.addSubtree(
        OctreeTileID{6, 0, 0, 0},
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));
}

TEST(TileOctreeAvailabilityTest, AddSubtreeRejectsNonBoundaryOrUnavailableChildLikeCesiumNative) {
    TileOctreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(
        OctreeTileID{0, 0, 0, 0},
        makeOctreeFixtureRootSubtree()));

    EXPECT_FALSE(availability.addSubtree(
        OctreeTileID{2, 0, 0, 0},
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));

    EXPECT_FALSE(availability.addSubtree(
        OctreeTileID{3, 2, 0, 3},
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));
}

TEST(TileOctreeAvailabilityTest, AddSubtreeRejectsOutOfRangeChildCoordinates) {
    TileOctreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(
        OctreeTileID{0, 0, 0, 0},
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            {}}));
    TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);

    EXPECT_FALSE(availability.addSubtree(
        OctreeTileID{3, 8, 0, 0},
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));
    EXPECT_FALSE(availability.addSubtree(
        OctreeTileID{3, 0, 8, 0},
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));
    EXPECT_FALSE(availability.addSubtree(
        OctreeTileID{3, 0, 0, 8},
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));
    EXPECT_EQ(nullptr, root->childNodes[0].get());
}

TEST(TileOctreeAvailabilityTest, AddNodeAndAddLoadedSubtreeMatchCesiumNative) {
    TileOctreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(
        OctreeTileID{0, 0, 0, 0},
        makeOctreeFixtureRootSubtree()));
    TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);

    const OctreeTileID id{3, 0, 1, 0};
    TileAvailabilityNode* node = availability.addNode(id, root);
    ASSERT_NE(nullptr, node);
    EXPECT_EQ(static_cast<uint8_t>(TileAvailable | SubtreeAvailable),
              availability.computeAvailability(id, node));

    EXPECT_TRUE(availability.addLoadedSubtree(
        node,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));
    EXPECT_FALSE(availability.addLoadedSubtree(
        node,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));

    const uint8_t loadedState = availability.computeAvailability(id);
    EXPECT_TRUE((loadedState & TileAvailable) != 0);
    EXPECT_TRUE((loadedState & SubtreeAvailable) != 0);
    EXPECT_TRUE((loadedState & SubtreeLoaded) != 0);
}

TEST(TileOctreeAvailabilityTest, AddNodeReplacesExistingChildNodeLikeCesiumNative) {
    TileOctreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(
        OctreeTileID{0, 0, 0, 0},
        makeOctreeFixtureRootSubtree()));
    TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);

    const OctreeTileID id{3, 0, 1, 0};
    TileAvailabilityNode* firstNode = availability.addNode(id, root);
    ASSERT_NE(nullptr, firstNode);
    ASSERT_TRUE(availability.addLoadedSubtree(
        firstNode,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));

    TileAvailabilityNode* secondNode = availability.addNode(id, root);
    ASSERT_NE(nullptr, secondNode);
    EXPECT_NE(firstNode, secondNode);
    EXPECT_EQ(secondNode, availability.findChildNode(id, root));

    const uint8_t replacedState = availability.computeAvailability(id);
    EXPECT_TRUE((replacedState & TileAvailable) != 0);
    EXPECT_TRUE((replacedState & SubtreeAvailable) != 0);
    EXPECT_FALSE((replacedState & SubtreeLoaded) != 0);
}

TEST(TileOctreeAvailabilityTest, AddSubtreeRejectsExistingEmptyNodeLikeCesiumNative) {
    TileOctreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(
        OctreeTileID{0, 0, 0, 0},
        makeOctreeFixtureRootSubtree()));
    TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);
    const OctreeTileID id{3, 0, 1, 0};
    ASSERT_NE(nullptr, availability.addNode(id, root));

    EXPECT_FALSE(availability.addSubtree(
        id,
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{false},
            {}}));
}

TEST(TileOctreeAvailabilityTest, AddNodeRejectsNonBoundaryOrUnavailableChildLikeCesiumNative) {
    TileOctreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(
        OctreeTileID{0, 0, 0, 0},
        makeOctreeFixtureRootSubtree()));
    TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);

    EXPECT_EQ(nullptr, availability.addNode(OctreeTileID{2, 0, 0, 0}, root));
    EXPECT_EQ(nullptr, availability.addNode(OctreeTileID{3, 2, 0, 3}, root));
}

TEST(TileOctreeAvailabilityTest, AddNodeRejectsOutOfRangeRootCoordinates) {
    TileOctreeAvailability availability(3, 5);

    EXPECT_EQ(nullptr,
              availability.addNode(OctreeTileID{0, 1, 0, 0}, nullptr));
    EXPECT_EQ(nullptr,
              availability.addNode(OctreeTileID{0, 0, 1, 0}, nullptr));
    EXPECT_EQ(nullptr,
              availability.addNode(OctreeTileID{0, 0, 0, 1}, nullptr));
    EXPECT_EQ(nullptr,
              availability.addNode(OctreeTileID{0, -1, 0, 0}, nullptr));
    EXPECT_EQ(nullptr, availability.rootNode());

    TileAvailabilityNode* root =
        availability.addNode(OctreeTileID{0, 0, 0, 0}, nullptr);
    EXPECT_NE(nullptr, root);
    EXPECT_EQ(root, availability.rootNode());
}

TEST(TileOctreeAvailabilityTest, AddNodeRejectsOutOfRangeChildCoordinates) {
    TileOctreeAvailability availability(3, 5);
    ASSERT_TRUE(availability.addSubtree(
        OctreeTileID{0, 0, 0, 0},
        TileAvailabilitySubtree{
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            ConstantTileAvailability{true},
            {}}));
    TileAvailabilityNode* root = availability.rootNode();
    ASSERT_NE(nullptr, root);

    EXPECT_EQ(std::nullopt,
              availability.findChildNodeIndex(OctreeTileID{3, 8, 0, 0}, root));
    EXPECT_EQ(std::nullopt,
              availability.findChildNodeIndex(OctreeTileID{3, 0, 8, 0}, root));
    EXPECT_EQ(std::nullopt,
              availability.findChildNodeIndex(OctreeTileID{3, 0, 0, 8}, root));
    EXPECT_EQ(nullptr, availability.addNode(OctreeTileID{3, 8, 0, 0}, root));
    EXPECT_EQ(nullptr, availability.addNode(OctreeTileID{3, 0, 8, 0}, root));
    EXPECT_EQ(nullptr, availability.addNode(OctreeTileID{3, 0, 0, 8}, root));
    EXPECT_EQ(512U, root->childNodes.size());
}
