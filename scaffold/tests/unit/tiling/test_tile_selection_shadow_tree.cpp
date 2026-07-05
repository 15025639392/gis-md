// Shadow-tree snapshot builder（异步选择 步1）单测。
//
// 契约：TileSelectionShadowTree::build 从 live registry 复制一棵并行影子树，
// 逐瓦片镜像"选择读取面"（几何/结构 + 载入/内容分类 + 上采样类别 +
// 跨帧选择历史）并保真复制 parent/children 拓扑（含 children 顺序）。
// 影子树是后续 worker 跑原样遍历的输入——镜像失真 = golden 失配，故此处
// 逐字段断言。glTF/raster 渲染内容读取面本步不镜像（见类文档），不断言。

#include <gtest/gtest.h>

#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileKey.h"
#include "earth_engine/tiling/TileBoundingVolume.h"
#include "earth_engine/tiling/TileSelectionShadowTree.h"
#include "earth_engine/tiling/TilesetTile.h"
#include "earth_engine/tiling/TilesetTileRegistry.h"

#include <memory>
#include <string>
#include <vector>

using namespace earth_engine;

namespace {

constexpr const char* kSchemeId = "Geographic-TMS";

// 直接插入 live registry（不经 ensureTile，避免依赖 scheme/provider）。
TilesetTile* insertLive(TilesetTileRegistry& registry,
                        const TileKey& key,
                        const Rectangle& bounds,
                        TilesetTile* parent) {
    auto tile = std::make_unique<TilesetTile>(key, bounds, parent);
    TilesetTile* raw = tile.get();
    registry.tiles()[TileCacheKey::forTile(key)] = std::move(tile);
    if (parent) {
        parent->children.push_back(raw);
    }
    return raw;
}

// 构造一棵内容各异的小树：root(z0) + 4 子(z1) + child0 下 2 孙(z2)。
// 每瓦片刻意赋不同 refine / loadState / contentKind / upsampleKind /
// 选择历史 / 包围体，逼出任何字段漏拷。
TilesetTileRegistry makeLiveTree() {
    TilesetTileRegistry registry;

    TileKey rootKey{kSchemeId, 0, 0, 0};
    TilesetTile* root =
        insertLive(registry, rootKey, Rectangle(-3.1, -1.5, 3.1, 1.5), nullptr);
    root->geometricError = 1000.0;
    root->refine = TileRefine::Replace;
    root->unconditionallyRefine = true;
    root->boundingVolume =
        TileBoundingVolume::fromLooseRegion(root->bounds, -100.0, 8000.0);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Empty;
    root->selectionFrameState.previousSelectionState =
        TileSelectionState::Refined;
    root->selectionFrameState.selectionState = TileSelectionState::Refined;

    for (int i = 0; i < 4; ++i) {
        const int dx = i % 2;
        const int dy = i / 2;
        TileKey childKey{kSchemeId, 1, dx, dy};
        const double w = -3.1 + dx * 3.1;
        const double s = -1.5 + dy * 1.5;
        TilesetTile* child = insertLive(
            registry,
            childKey,
            Rectangle(w, s, w + 3.1, s + 1.5),
            root);
        child->geometricError = 500.0 + i;
        child->refine = (i % 2 == 0) ? TileRefine::Replace : TileRefine::Add;
        child->unconditionallyRefine = false;
        child->boundingVolume =
            TileBoundingVolume::fromRegion(child->bounds, 0.0, 10.0 + i);
        child->contentBoundingVolume =
            TileBoundingVolume::fromRegion(child->bounds, 1.0, 5.0);
        child->content.loadState =
            (i == 0) ? TileLoadState::ContentLoading : TileLoadState::Unloaded;
        child->content.contentKind =
            (i == 0) ? TileContentKind::Render : TileContentKind::Unknown;
        if (i == 3) {
            child->content.contentUpsampleKind =
                TileContentUpsampleKind::TerrainAvailability;
        }
        child->selectionFrameState.previousSelectionState =
            (i == 0) ? TileSelectionState::Rendered
                     : TileSelectionState::NotVisited;
        child->selectionFrameState.selectionState =
            (i == 0) ? TileSelectionState::Rendered
                     : TileSelectionState::Culled;
    }

    // child0 (z1,0,0) 下挂 2 个孙（不是全 4 个——验证部分子集拓扑）。
    TilesetTile* child0 = registry.findTile(TileKey{kSchemeId, 1, 0, 0});
    for (int i = 0; i < 2; ++i) {
        TileKey g{kSchemeId, 2, i, 0};
        const double w = -3.1 + i * 1.55;
        TilesetTile* grand = insertLive(
            registry,
            g,
            Rectangle(w, -1.5, w + 1.55, -0.75),
            child0);
        grand->geometricError = 250.0 + i;
        grand->refine = TileRefine::Replace;
        grand->content.loadState = TileLoadState::Done;
        grand->content.contentKind = TileContentKind::Render;
        grand->content.contentUpsampleKind =
            TileContentUpsampleKind::RasterDetail;
        grand->selectionFrameState.previousSelectionState =
            TileSelectionState::RenderedAndKicked;
        grand->selectionFrameState.selectionState =
            TileSelectionState::Rendered;
    }

    return registry;
}

std::optional<Rectangle> regionOf(
    const std::optional<TileBoundingVolume>& bv) {
    if (!bv) {
        return std::nullopt;
    }
    return bv->region;
}

void expectReadSurfaceMirrors(const TilesetTile& live,
                              const TilesetTile& shadow) {
    SCOPED_TRACE("tile z=" + std::to_string(live.key.z) + " x=" +
                 std::to_string(live.key.x) + " y=" +
                 std::to_string(live.key.y));
    EXPECT_EQ(shadow.key, live.key);
    EXPECT_EQ(shadow.bounds.west(), live.bounds.west());
    EXPECT_EQ(shadow.bounds.south(), live.bounds.south());
    EXPECT_EQ(shadow.bounds.east(), live.bounds.east());
    EXPECT_EQ(shadow.bounds.north(), live.bounds.north());
    EXPECT_EQ(shadow.geometricError, live.geometricError);
    EXPECT_EQ(shadow.refine, live.refine);
    EXPECT_EQ(shadow.unconditionallyRefine, live.unconditionallyRefine);

    EXPECT_EQ(shadow.boundingVolume.has_value(),
              live.boundingVolume.has_value());
    EXPECT_EQ(regionOf(shadow.boundingVolume), regionOf(live.boundingVolume));
    EXPECT_EQ(shadow.contentBoundingVolume.has_value(),
              live.contentBoundingVolume.has_value());
    EXPECT_EQ(regionOf(shadow.contentBoundingVolume),
              regionOf(live.contentBoundingVolume));

    EXPECT_EQ(shadow.content.loadState, live.content.loadState);
    EXPECT_EQ(shadow.content.contentKind, live.content.contentKind);
    EXPECT_EQ(shadow.content.contentUpsampleKind,
              live.content.contentUpsampleKind);

    EXPECT_EQ(shadow.selectionFrameState.previousSelectionState,
              live.selectionFrameState.previousSelectionState);
    EXPECT_EQ(shadow.selectionFrameState.selectionState,
              live.selectionFrameState.selectionState);
}

} // namespace

TEST(TileSelectionShadowTreeTest, MirrorsReadSurfaceForEveryTile) {
    TilesetTileRegistry live = makeLiveTree();

    TileSelectionShadowTree shadowTree;
    shadowTree.build(live);

    EXPECT_EQ(shadowTree.size(), live.tiles().size());

    for (const auto& entry : live.tiles()) {
        const TilesetTile& liveTile = *entry.second;
        const TilesetTile* shadow = shadowTree.findShadow(liveTile.key);
        ASSERT_NE(shadow, nullptr)
            << "shadow 缺瓦片 z=" << liveTile.key.z;
        expectReadSurfaceMirrors(liveTile, *shadow);
    }
}

TEST(TileSelectionShadowTreeTest, MirrorsTopologyByKeyAndOrder) {
    TilesetTileRegistry live = makeLiveTree();

    TileSelectionShadowTree shadowTree;
    shadowTree.build(live);

    for (const auto& entry : live.tiles()) {
        const TilesetTile& liveTile = *entry.second;
        const TilesetTile* shadow = shadowTree.findShadow(liveTile.key);
        ASSERT_NE(shadow, nullptr);

        // parent 按 key 指向影子父（而非 live 父指针）。
        if (liveTile.parent) {
            ASSERT_NE(shadow->parent, nullptr);
            EXPECT_EQ(shadow->parent->key, liveTile.parent->key);
            EXPECT_EQ(shadow->parent,
                      shadowTree.findShadow(liveTile.parent->key));
        } else {
            EXPECT_EQ(shadow->parent, nullptr);
        }

        // children：数量 + 顺序 + 每个都是影子实例（不指向 live）。
        ASSERT_EQ(shadow->children.size(), liveTile.children.size());
        for (size_t i = 0; i < liveTile.children.size(); ++i) {
            const TilesetTile* liveChild = liveTile.children[i];
            const TilesetTile* shadowChild = shadow->children[i];
            ASSERT_NE(shadowChild, nullptr);
            EXPECT_EQ(shadowChild->key, liveChild->key);
            EXPECT_EQ(shadowChild, shadowTree.findShadow(liveChild->key));
            // 影子 child 的 parent 回指本影子瓦片（拓扑自洽）。
            EXPECT_EQ(shadowChild->parent, shadow);
        }
    }
}

TEST(TileSelectionShadowTreeTest, RebuildClearsStalePriorContent) {
    TilesetTileRegistry live = makeLiveTree();
    TileSelectionShadowTree shadowTree;
    shadowTree.build(live);
    const size_t firstSize = shadowTree.size();
    EXPECT_GT(firstSize, 0u);

    // 用更小的树重建：不得残留上一棵的瓦片。
    TilesetTileRegistry smaller;
    insertLive(smaller, TileKey{kSchemeId, 0, 0, 0},
               Rectangle(-3.1, -1.5, 3.1, 1.5), nullptr);
    shadowTree.build(smaller);

    EXPECT_EQ(shadowTree.size(), 1u);
    EXPECT_NE(shadowTree.findShadow(TileKey{kSchemeId, 0, 0, 0}), nullptr);
    EXPECT_EQ(shadowTree.findShadow(TileKey{kSchemeId, 1, 0, 0}), nullptr);
}
