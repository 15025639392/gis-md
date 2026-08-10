#pragma once

#include "TileKey.h"
#include "TerrainEdgeLutTable.h"
#include <array>
#include <string>
#include <vector>

namespace earth_engine {

struct TilesetTile;

/// TilePlan is the frame-derived selection result for one tile scheme. The
/// selector owns visible/fading tile decisions and resolves the render entries
/// that the renderer consumes without reselecting LOD.
enum class TileRenderEntryReason {
    Direct,
    AncestorFallback,
    SynchronousPrep
};

enum class TileRenderEntryPass {
    Selected,
    Fading
};

constexpr const char* tileRenderEntryReasonLabel(
    TileRenderEntryReason reason) {
    switch (reason) {
        case TileRenderEntryReason::Direct:
            return "direct";
        case TileRenderEntryReason::AncestorFallback:
            return "ancestor-fallback";
        case TileRenderEntryReason::SynchronousPrep:
            return "sync-prep";
    }
    return "unknown";
}

/// A resolved entry in the frame render result. `selectedKey` is the tile
/// chosen by traversal; `renderKey` is the tile whose render content should be
/// submitted. They differ only for explicit, clipped ancestor fallback.
struct TileRenderEntry {
    TileKey selectedKey;
    TileKey renderKey;
    TileRenderEntryReason reason = TileRenderEntryReason::Direct;
    float opacity = 1.0f;
    bool selectedThisFrame = true;
    bool usesAncestorFallback = false;
    bool allowSynchronousMeshPrep = true;
    bool surfaceClipEnabled = false;
    std::array<float, 4> surfaceClipUv{0.0f, 0.0f, 1.0f, 1.0f};
    TilesetTile* selectedTile = nullptr;
    TilesetTile* renderTile = nullptr;

    bool isAncestorFallback() const {
        return reason == TileRenderEntryReason::AncestorFallback &&
               usesAncestorFallback;
    }

    bool hasSurfaceClip() const { return surfaceClipEnabled; }

    TileRenderEntryPass renderPass() const {
        return selectedThisFrame ? TileRenderEntryPass::Selected
                                 : TileRenderEntryPass::Fading;
    }
};

/// 机制 B ①-1:一个吸附瓦片的 4 条边各自的邻居来源。TileEdgeSnapResolver 在
/// 解析八度差时顺手记下命中的邻居 entry —— 边高度 LUT 需要"邻居实际渲染出来
/// 的高度",而那要邻居的 heightmap/bounds/档位/morph,只有 entry 能给全。
///
/// 条目生存期 = 本帧渲染集(entry 指针指向同一个 renderEntries 向量的元素),
/// 消费者(TileEdgeHeightLutBuilder)在同帧内跑完即弃,不跨帧持有。
struct TileEdgeSnapRecord {
    static constexpr int kEdgeCount = 4;  // W, E, N, S(与 shader 打包序一致)

    TilesetTile* tile = nullptr;
    // 每边的 log2 吸附步长(0 = 该边不吸附)。冗余于 edgeSnapPacked,但拆开
    // 存是为了 LUT 构建端不必再解一次打包(解包写两遍必然有一遍会写错)。
    int edgeLog2[kEdgeCount] = {0, 0, 0, 0};
    const TileRenderEntry* neighbor[kEdgeCount] = {nullptr, nullptr, nullptr,
                                                   nullptr};

    bool hasAnySnap() const {
        for (int i = 0; i < kEdgeCount; ++i) {
            if (edgeLog2[i] > 0 && neighbor[i]) {
                return true;
            }
        }
        return false;
    }
};

enum class TileSelectionState {
    NotVisited,
    Culled,
    Rendered,
    Refined,
    RenderedAndKicked,
    RefinedAndKicked
};

constexpr bool selectionWasKicked(TileSelectionState state) {
    return state == TileSelectionState::RenderedAndKicked ||
           state == TileSelectionState::RefinedAndKicked;
}

constexpr TileSelectionState originalSelectionState(TileSelectionState state) {
    switch (state) {
        case TileSelectionState::RenderedAndKicked:
            return TileSelectionState::Rendered;
        case TileSelectionState::RefinedAndKicked:
            return TileSelectionState::Refined;
        default:
            return state;
    }
}

constexpr void kickSelectionState(TileSelectionState& state) {
    switch (state) {
        case TileSelectionState::Rendered:
            state = TileSelectionState::RenderedAndKicked;
            break;
        case TileSelectionState::Refined:
            state = TileSelectionState::RefinedAndKicked;
            break;
        default:
            break;
    }
}

struct TileSelectionRecord {
    TileKey key;
    TileSelectionState state = TileSelectionState::NotVisited;
    TileSelectionState previousState = TileSelectionState::NotVisited;
    double screenSpaceError = 0.0;
    bool cameraInside = false;
    bool inFrustum = false;
    bool ancestorMeetsSse = false;
};

struct TilePlan {
    TilePlan() = default;
    TilePlan(const TilePlan&) = delete;
    TilePlan& operator=(const TilePlan&) = delete;
    TilePlan(TilePlan&&) = default;
    TilePlan& operator=(TilePlan&&) = default;

    uint64_t frameId = 0;
    int zoom = 0;
    int minVisibleZoom = 0;
    int maxVisibleZoom = 0;
    bool equalZoomApplied = false;
    double lodSizePixels = 0.0;
    double minLodSizePixels = 0.0;
    double maxLodSizePixels = 0.0;
    std::vector<TileKey> visibleTiles;
    // Cesium Native ViewUpdateResult carries intrusive Tile pointers. The
    // selector appends these live handles with visibleTiles; traversal
    // references protect them through finalization, and render-frame
    // references protect them through command submission.
    std::vector<TilesetTile*> tilesToRenderThisFrame;
    std::vector<TileRenderEntry> renderEntries;
    // 机制 B ①-1:本帧有吸附边的瓦片及其邻居来源(TileEdgeSnapResolver 产,
    // TileEdgeHeightLutBuilder 消费)。renderEntries 定稿后才填,故其中的
    // entry 指针在本帧内稳定。
    std::vector<TileEdgeSnapRecord> edgeSnapRecords;
    // ①-1(A′):吸附瓦片的边高度差表,refresher 在 resolve 后从
    // edgeSnapRecords 就地建成纯数据(邻居指针同帧消费掉,不跨阶段)。
    // draw 侧按 terrainEdgeCellKey(tile.key) 查表上传。
    TerrainEdgeLutTableMap edgeLutTables;
    std::vector<std::string> frameCredits;
    int frameMappedRasterTileCount = 0;
    int frameMappedRasterTileLoadingCount = 0;
    int frameProgressTotalCount = 0;
    int frameProgressLoadingCount = 0;
    double frameLoadProgressPercentage = 100.0;
    std::vector<TileSelectionRecord> selectionRecords;
    int renderingNodeCount = 0;
    int walkthroughNodeCount = 0;
    int notRenderingNodeCount = 0;
    int selectionRenderedCount = 0;
    int selectionRefinedCount = 0;
    int selectionKickedCount = 0;
    int selectionOccludedCount = 0;
    int selectionWaitingForOcclusionResultsCount = 0;
    int culledTilesVisitedCount = 0;
    int selectionAncestorMeetsSseCount = 0;
    int cameraInsideNodeCount = 0;
    int inFrustumNodeCount = 0;
    int horizonTangentPreservedCount = 0;
    int equalZoomSecondPassNodeCount = 0;
    int neighborLinkCount = 0;
    int neighborBalancedTileCount = 0;
    int mercatorTileCount = 0;
    int northPolarTileCount = 0;
    int southPolarTileCount = 0;
    int renderEntryAncestorFallbackCount = 0;
    int renderEntrySynchronousPrepCount = 0;
    int renderEntryDeferredPrepCount = 0;
    int renderEntryPlannedCommandCount = 0;
    int renderEntrySelectedPlannedCommandCount = 0;
    int renderEntryCommandDrawCount = 0;
    int renderEntrySelectedCommandDrawCount = 0;
    int renderEntryCommandMissedDrawCount = 0;
    int renderEntrySelectedCommandMissedDrawCount = 0;
    int renderEntryCommandMissingSelectedCount = 0;
    int renderEntryCommandMissingRenderCount = 0;
    int renderEntryCommandDeferredCount = 0;
    int renderEntrySelectedCommandDeferredCount = 0;
    // 破洞诊断:零命令的成因分桶(见 TileRenderCommandZeroDrawBreakdown)。
    // 三者之和 ≈ renderEntryCommandMissedDrawCount。
    // 破洞诊断:选中瓦片被 finalizer 丢弃、且不是"已被别的 entry 覆盖"的
    // dedup —— 这两类才是屏幕上真的没有几何。
    int renderEntryDropClipUvCount = 0;
    int renderEntryDropNotBuildableCount = 0;
    // NotBuildable 的成因分桶(定「补影像兜底」的范围用):几何就没有 /
    // 没建 mapping / 建了但无可用纹理(含祖先)/ texcoord 越界 / 其它。
    int renderEntryDropNoGeometryCount = 0;
    int renderEntryDropNoMappingCount = 0;
    int renderEntryDropNoReadyTextureCount = 0;
    int renderEntryDropTexcoordInvalidCount = 0;
    int renderEntryDropOtherCount = 0;
    int renderEntryDropMinZoom = 0;
    int renderEntryDropMaxZoom = 0;
    // 本帧第一片 NoReadyTexture 丢弃的画像(见 BaseImageryNoTextureProbe)。
    // 取快照而非累加:每帧这类丢弃只有 1-4 片且多为同源兄弟,看一片的完整
    // 病历比看四片的和更能定因。
    int renderEntryDropNoTexZoom = -1;
    int renderEntryDropNoTexLoadingState = -1;
    int renderEntryDropNoTexReadyState = -1;
    int renderEntryDropNoTexReadyHasTexture = 0;
    int renderEntryDropNoTexAncestorDepth = 0;
    int renderEntryDropNoTexAncestorsWithMapping = 0;
    int renderEntryDropNoTexAncestorsWithTexture = 0;
    int renderEntryDropNoTexMappingState = -1;
    unsigned long long renderEntryDropNoTexAuthoritativeUpdates = 0;
    int renderEntryDropNoTexTileLoadState = -99;
    int renderEntryDropNoTexTileContentKind = -1;
    int renderEntryZeroDrawNoContentNoFillCount = 0;
    int renderEntryZeroDrawFillNoCommandsCount = 0;
    int renderEntryZeroDrawContentNoCommandsCount = 0;
};

class TilePlanBuilder {
public:
    static TileKey parentKey(const TileKey& key);
};

} // namespace earth_engine
