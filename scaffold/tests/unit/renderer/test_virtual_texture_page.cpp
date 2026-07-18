#include <gtest/gtest.h>

#include "earth_engine/renderer/VirtualTexturePage.h"

#include <vector>

using namespace earth_engine;

namespace {

std::vector<uint8_t> pixelsFor(const std::vector<VtPageId>& pages,
                               int width,
                               int height) {
    std::vector<uint8_t> px(static_cast<size_t>(width) * height * 4u, 0);
    size_t i = 0;
    for (const VtPageId& p : pages) {
        const auto rgba = vt_codec::encode(p);
        const size_t off = (i % (static_cast<size_t>(width) * height)) * 4u;
        px[off + 0] = rgba[0];
        px[off + 1] = rgba[1];
        px[off + 2] = rgba[2];
        px[off + 3] = rgba[3];
        ++i;
    }
    return px;
}

}  // namespace

// ---- codec ----

TEST(VirtualTexturePageCodec, PackUnpackRoundtripWithinBudget) {
    for (uint32_t lod = 0; lod <= vt_codec::kMaxLod; ++lod) {
        VtPageId p{123u, 4567u, lod};
        const uint32_t key = vt_codec::packKey(p);
        const VtPageId back = vt_codec::unpackKey(key);
        EXPECT_EQ(back.lod, lod);
        EXPECT_EQ(back.x, 123u);
        EXPECT_EQ(back.y, 4567u);
    }
}

TEST(VirtualTexturePageCodec, EncodeDecodeRgba8Roundtrip) {
    VtPageId p{8191u, 16383u, 14u};
    const auto rgba = vt_codec::encode(p);
    const uint32_t key = vt_codec::decodeKey(rgba[0], rgba[1], rgba[2], rgba[3]);
    const VtPageId back = vt_codec::unpackKey(key);
    EXPECT_EQ(back, p);
}

TEST(VirtualTexturePageCodec, OutOfBudgetCoordinatesClampNotOverflow) {
    // 超 14 位坐标被 clamp,不串位到 lod/邻页。
    VtPageId p{99999u, 99999u, 99u};
    const VtPageId back = vt_codec::unpackKey(vt_codec::packKey(p));
    EXPECT_EQ(back.lod, vt_codec::kMaxLod);
    EXPECT_EQ(back.x, vt_codec::kMaxCoord);
    EXPECT_EQ(back.y, vt_codec::kMaxCoord);
}

// ---- feedback 解码 ----

TEST(VirtualTexturePageDecode, DedupesAndSkipsBackground) {
    std::vector<VtPageId> pages{{1u, 1u, 5u}, {1u, 1u, 5u}, {2u, 3u, 5u}};
    auto px = pixelsFor(pages, 8, 8);  // 其余像素全 0 = 背景
    const std::vector<VtPageId> got =
        decodeFeedbackPixels(px.data(), 8, 8, 8u * 4u);
    ASSERT_EQ(got.size(), 2u);  // 去重后 2 页,背景略过
    EXPECT_EQ(got[0], (VtPageId{1u, 1u, 5u}));
    EXPECT_EQ(got[1], (VtPageId{2u, 3u, 5u}));
}

TEST(VirtualTexturePageDecode, EmptyOnNullOrZeroSize) {
    EXPECT_TRUE(decodeFeedbackPixels(nullptr, 8, 8, 32).empty());
    std::vector<uint8_t> px(4, 0);
    EXPECT_TRUE(decodeFeedbackPixels(px.data(), 0, 0, 0).empty());
}

// ---- 页表 ----

TEST(VirtualTexturePageTable, ResidentUntilCapacityThenLruEvicts) {
    VtPageTable table(2, 1);  // 容量 2
    EXPECT_EQ(table.capacity(), 2);

    std::vector<VtIndirectionUpdate> updates;
    VtPageTableStats s =
        table.registerVisible({{0u, 0u, 5u}, {1u, 0u, 5u}}, &updates);
    EXPECT_EQ(s.visibleCount, 2);
    EXPECT_EQ(s.newlyResident, 2);
    EXPECT_EQ(s.residentCount, 2);
    EXPECT_FALSE(s.thrashed);
    EXPECT_EQ(updates.size(), 2u);
    EXPECT_TRUE(table.isResident({0u, 0u, 5u}));

    // 第三页可见、前两页不可见 → 淘汰 LRU(最久未见的 page0),page1 仍在。
    updates.clear();
    s = table.registerVisible({{2u, 0u, 5u}}, &updates);
    EXPECT_EQ(s.newlyResident, 1);
    EXPECT_EQ(s.evicted, 1);
    EXPECT_EQ(s.residentCount, 2);
    EXPECT_TRUE(table.isResident({2u, 0u, 5u}));
    EXPECT_FALSE(table.isResident({0u, 0u, 5u}));  // 被淘汰
}

TEST(VirtualTexturePageTable, VisiblePagesProtectedFromEviction) {
    VtPageTable table(2, 1);
    table.registerVisible({{0u, 0u, 5u}, {1u, 0u, 5u}});
    // 两页都占满,新页可见但两旧页也都可见 → 无可淘汰者(thrash),新页调不进。
    VtPageTableStats s = table.registerVisible(
        {{0u, 0u, 5u}, {1u, 0u, 5u}, {2u, 0u, 5u}});
    EXPECT_TRUE(s.thrashed);
    EXPECT_EQ(s.residentCount, 2);
    EXPECT_FALSE(table.isResident({2u, 0u, 5u}));
}

TEST(VirtualTexturePageTable, StableSlotAcrossFramesNoNeedlessUpdate) {
    VtPageTable table(4, 1);
    std::vector<VtIndirectionUpdate> updates;
    table.registerVisible({{0u, 0u, 5u}, {1u, 0u, 5u}}, &updates);
    ASSERT_EQ(updates.size(), 2u);

    VtAtlasSlot slot0Before;
    ASSERT_TRUE(table.slotOf({0u, 0u, 5u}, slot0Before));

    // 同页再次可见 → 不产生新 update(槽不变),且槽稳定。
    updates.clear();
    table.registerVisible({{0u, 0u, 5u}, {1u, 0u, 5u}}, &updates);
    EXPECT_TRUE(updates.empty());
    VtAtlasSlot slot0After;
    ASSERT_TRUE(table.slotOf({0u, 0u, 5u}, slot0After));
    EXPECT_EQ(slot0After.column, slot0Before.column);
    EXPECT_EQ(slot0After.row, slot0Before.row);
}

TEST(VirtualTexturePageTable, ResetClearsResidency) {
    VtPageTable table(2, 2);
    table.registerVisible({{0u, 0u, 5u}, {1u, 0u, 5u}});
    EXPECT_EQ(table.residentCount(), 2);
    table.reset();
    EXPECT_EQ(table.residentCount(), 0);
    EXPECT_FALSE(table.isResident({0u, 0u, 5u}));
    // reset 后槽重新从头分配。
    std::vector<VtIndirectionUpdate> updates;
    table.registerVisible({{9u, 9u, 6u}}, &updates);
    ASSERT_EQ(updates.size(), 1u);
    EXPECT_EQ(updates[0].slot.column, 0);
    EXPECT_EQ(updates[0].slot.row, 0);
}
