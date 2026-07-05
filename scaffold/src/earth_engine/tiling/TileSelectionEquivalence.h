#pragma once

#include "TileLoadPriorityPolicy.h"
#include "TileLoadQueue.h"
#include "TileLoadTypes.h"
#include "TilePlan.h"
#include "TileSelectionCounters.h"
#include "TileKey.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace earth_engine {

// §8 等价性基座(见 docs/issues/selector-incremental-frontier-design-2026-07-06.md)。
// 比较两次选择的产出是否等价,用于 ③ 增量切面的 oracle:每帧「增量选择」的
// 结果必须与「全量选择」逐位相同。等价关系刻意对齐 golden traceLine 的权威
// 字段集(render 集 + priority 排序后的 load 队列 + 计数),并再加全部
// counters(强于 golden 的 4 项)。第一处不一致即返回,带可读描述。
//
// 纯比较、无副作用、header-only —— 既供运行期 debug 断言,也供单测直接调用。
struct TileSelectionEquivalence {
    struct Result {
        bool equal = true;
        std::string mismatch;  // 空 = 相等;否则为第一处差异的可读描述
    };

    // render 集合语义:visibleTiles 顺序对不透明地形无意义(深度缓冲裁决),
    // 故按 (schemeId,z,x,y) 排序后作为多重集合比较——与 golden 的
    // joinSorted(renderKeys) 一致。
    static bool keyLess(const TileKey& a, const TileKey& b) {
        if (a.schemeId != b.schemeId) return a.schemeId < b.schemeId;
        if (a.z != b.z) return a.z < b.z;
        if (a.x != b.x) return a.x < b.x;
        return a.y < b.y;
    }

    static std::string keyStr(const TileKey& k) {
        std::ostringstream os;
        os << k;
        return os.str();
    }

    static Result compare(const TilePlan& planA,
                          const TileLoadQueue& loadA,
                          const TileSelectionCounters& countersA,
                          const TilePlan& planB,
                          const TileLoadQueue& loadB,
                          const TileSelectionCounters& countersB) {
        Result r;

        // ---- 1. render 集(visibleTiles),排序后多重集合比较 ----
        std::vector<TileKey> renderA = planA.visibleTiles;
        std::vector<TileKey> renderB = planB.visibleTiles;
        std::sort(renderA.begin(), renderA.end(), keyLess);
        std::sort(renderB.begin(), renderB.end(), keyLess);
        if (renderA.size() != renderB.size()) {
            r.equal = false;
            r.mismatch = "visibleTiles size " +
                         std::to_string(renderA.size()) + " vs " +
                         std::to_string(renderB.size());
            return r;
        }
        for (size_t i = 0; i < renderA.size(); ++i) {
            if (!(renderA[i] == renderB[i])) {
                r.equal = false;
                r.mismatch = "visibleTiles[" + std::to_string(i) + "] " +
                             keyStr(renderA[i]) + " vs " + keyStr(renderB[i]);
                return r;
            }
        }

        // ---- 2. loadQueue,按生产优先级排序后逐项比较(key+group+priority) ----
        std::vector<TileLoadRequest> reqA = loadA.requests();
        std::vector<TileLoadRequest> reqB = loadB.requests();
        TileLoadPriorityPolicy::sortByPriority(reqA);
        TileLoadPriorityPolicy::sortByPriority(reqB);
        if (reqA.size() != reqB.size()) {
            r.equal = false;
            r.mismatch = "loadQueue size " + std::to_string(reqA.size()) +
                         " vs " + std::to_string(reqB.size());
            return r;
        }
        for (size_t i = 0; i < reqA.size(); ++i) {
            if (!(reqA[i].key == reqB[i].key) ||
                reqA[i].group != reqB[i].group ||
                reqA[i].priority != reqB[i].priority) {
                r.equal = false;
                r.mismatch = "loadQueue[" + std::to_string(i) + "] " +
                             keyStr(reqA[i].key) + " vs " + keyStr(reqB[i].key);
                return r;
            }
        }

        // ---- 3. 全部选择计数(强于 golden 的 4 项) ----
        r = compareCounters(countersA, countersB);
        return r;
    }

private:
    static Result compareCounters(const TileSelectionCounters& a,
                                  const TileSelectionCounters& b) {
        Result r;
        const std::pair<const char*, std::pair<int, int>> fields[] = {
            {"visited", {a.visited, b.visited}},
            {"culled", {a.culled, b.culled}},
            {"kicked", {a.kicked, b.kicked}},
            {"fogCulled", {a.fogCulled, b.fogCulled}},
            {"notYetRenderable", {a.notYetRenderable, b.notYetRenderable}},
            {"culledVisited", {a.culledVisited, b.culledVisited}},
            {"occluded", {a.occluded, b.occluded}},
            {"waitingForOcclusionResults",
             {a.waitingForOcclusionResults, b.waitingForOcclusionResults}},
        };
        for (const auto& f : fields) {
            if (f.second.first != f.second.second) {
                r.equal = false;
                r.mismatch = std::string("counter ") + f.first + " " +
                             std::to_string(f.second.first) + " vs " +
                             std::to_string(f.second.second);
                return r;
            }
        }
        return r;
    }
};

} // namespace earth_engine
