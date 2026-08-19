#pragma once

#include "TileChildMaterializer.h"
#include "TileLoadDomainPolicy.h"
#include "TilesetTile.h"

#include <vector>

namespace earth_engine {

class IPrepareRendererResources;

struct TileChildFrameMaterializeInput {
    TilesetTile& tile;
    std::vector<TileKey> contentChildKeys;
    int maxZoom = 0;
    bool hasTerrainQuadtree = false;
    bool isAvailabilityBoundaryWaitingForContent = false;
    bool contentProviderOwnsTerrainQuadtree = false;
    IPrepareRendererResources* pPrepRenderer = nullptr;
    uint64_t childTopologyRevision = 0;
};

struct TileChildFrameMaterializeResult {
    bool changed = false;
    bool retryLater = false;
    bool fastPath = false;
};

class TileChildFrameMaterializer {
public:
    template <typename EnsureTileFn, typename AvailabilityStateFn>
    static TileChildFrameMaterializeResult ensureChildren(
        TileChildFrameMaterializeInput input,
        EnsureTileFn&& ensureTile,
        AvailabilityStateFn&& availabilityState) {
        const bool hasReliableTopologyVersion =
            input.childTopologyRevision != 0;
        const uint64_t configuration =
            materializationConfiguration(input);
        if (hasReliableTopologyVersion &&
            input.tile.childMaterializationStateValid &&
            input.tile.appliedChildTopologyRevision ==
                input.childTopologyRevision &&
            input.tile.appliedChildMaterializationInputRevision ==
                input.tile.childMaterializationInputRevision &&
            input.tile.appliedChildMaterializationConfiguration ==
                configuration) {
            return TileChildFrameMaterializeResult{false, false, true};
        }

        // 非完成态背压:上次尝试因子瓦片未就绪而未完成,此后输入/拓扑 revision
        // 都没变 ⇒ 内容没到,重走结果必然相同。直接早退(不跑 ensure/availability,
        // 不建任务),等子瓦片内容/包围体到达 bump 输入 revision 再恢复 ——
        // 消灭「加载中瓦片每帧全量重走」的白跑(真机 4 瓦/帧)。availability
        // boundary 分支(等待内容解析)不走这里:它每次都要重新排队 urgent 加载。
        if (hasReliableTopologyVersion &&
            !input.tile.childMaterializationStateValid &&
            input.tile.lastChildMaterializationAttemptInputRevision ==
                input.tile.childMaterializationInputRevision &&
            input.tile.lastChildMaterializationAttemptTopology ==
                input.childTopologyRevision) {
            return TileChildFrameMaterializeResult{false, false, false};
        }

        TileChildFrameMaterializeResult result;
        bool materializationComplete = true;
        if (!input.contentChildKeys.empty()) {
            result = TileChildFrameMaterializeResult{
                TileChildMaterializer::linkContentChildren(
                    input.tile,
                    input.contentChildKeys,
                    ensureTile,
                    &materializationComplete),
                false};
        } else if (!TileLoadDomainPolicy::shouldCreateTerrainChildren(
                       input.tile.key.z,
                       input.maxZoom,
                       input.hasTerrainQuadtree,
                       input.tile.content.isTerrainAvailabilityUpsample())) {
            result = {};
        } else if (input.isAvailabilityBoundaryWaitingForContent) {
            input.tile.childMaterializationStateValid = false;
            return TileChildFrameMaterializeResult{false, true, false};
        } else {
            result = TileChildFrameMaterializeResult{
                TileChildMaterializer::materializeTerrainChildren(
                    input.tile,
                    input.maxZoom,
                    availabilityState,
                    ensureTile,
                    input.contentProviderOwnsTerrainQuadtree,
                    input.pPrepRenderer,
                    &materializationComplete),
                false};
        }

        if (hasReliableTopologyVersion && materializationComplete) {
            input.tile.appliedChildTopologyRevision =
                input.childTopologyRevision;
            input.tile.appliedChildMaterializationInputRevision =
                input.tile.childMaterializationInputRevision;
            input.tile.appliedChildMaterializationConfiguration =
                configuration;
            input.tile.childMaterializationStateValid = true;
        } else {
            input.tile.childMaterializationStateValid = false;
            // 记录本次尝试的 revision:下次进来若仍未变则被上方背压早退。
            // 仅可靠拓扑版本走背压(无版本时保持旧行为:每帧重试,防旧 provider
            // 不 bump revision 导致子瓦片永不物化)。
            if (hasReliableTopologyVersion) {
                input.tile.lastChildMaterializationAttemptInputRevision =
                    input.tile.childMaterializationInputRevision;
                input.tile.lastChildMaterializationAttemptTopology =
                    input.childTopologyRevision;
            }
        }
        return result;
    }

private:
    static void hashCombine(uint64_t& seed, uint64_t value) {
        seed ^= value + 0x9e3779b97f4a7c15ULL +
                (seed << 6U) + (seed >> 2U);
    }

    static uint64_t materializationConfiguration(
        const TileChildFrameMaterializeInput& input) {
        uint64_t signature = 0xcbf29ce484222325ULL;
        hashCombine(signature, static_cast<uint64_t>(input.maxZoom));
        hashCombine(signature, input.hasTerrainQuadtree ? 1U : 0U);
        hashCombine(
            signature,
            input.isAvailabilityBoundaryWaitingForContent ? 1U : 0U);
        hashCombine(
            signature,
            input.contentProviderOwnsTerrainQuadtree ? 1U : 0U);
        return signature;
    }
};

} // namespace earth_engine
