#pragma once

#include "TileKey.h"

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace earth_engine {

/// 无缝北极星 ①-1(A′ 形态):边高度差表的**跨阶段载体** —— 纯数据,零指针。
///
/// 为什么必须是纯数据:此前 draw 侧拿着 resolve 盖章的记录(内含
/// TileRenderEntry*/TilesetTile* 裸指针)在消费时才采样邻居 heightmap,指针的
/// 有效性靠「resolve 每帧撤章」协议保证 —— 该协议已两次被证伪
/// (①记录指针悬垂;②Strict reuse 帧跳过 resolve,撤章不发生,瓦片却可在上一帧
/// draw 末尾的缓存淘汰中被析构 → 真机 SIGSEGV,tombstone_16..20 同一签名)。
/// 表在 resolve 阶段就地建好、按值存进 TilePlan,邻居指针在产生它们的同一阶段
/// 消费掉;draw 只按 key 查表,瓦片死活与它无关 —— 整类悬垂从机制上消失,
/// 「撤章」协议不再存在。
///
/// gridSize 语义:建表时采用的**无迟滞预测档**(terrainGridSizeForSse 单参)。
/// draw 实际 acquire 档与之不符(迟滞带保档/dense 池回落)时跳过上传,shader
/// 退回自纹理吸附(改前行为)。真机实测(2026-08-10)不符率:静止全零,变档
/// 途中 ≈2%(predM1=4/203),predM2=0 —— 可接受,由 SeamDiag edgeLut 行的
/// predM1/predM2 持续观测。
struct TerrainEdgeLutTable {
    static constexpr int kEdges = 4;      // W, E, N, S(与 shader 打包序一致)
    /// 最大节点数:dense 档(gridN=256)邻居粗一个八度(step=2)→ 256/2+1。
    static constexpr int kMaxNodes = 129;

    /// 存**差值**:邻居在该节点渲染出的高度 − 本瓦片该纹素渲染出的高度。
    /// shader 侧 hA/hB 照旧取自纹理再加差 → 两侧在共享边上求值同一个函数。
    /// 差值形式让失效路径天然安全:delta=0 = 改前行为。
    float delta[kEdges][kMaxNodes] = {};
    /// 0 = 该边不吸附或邻居数据取不到 → shader 退回自纹理吸附。
    int nodeCount[kEdges] = {};
    /// 建表用的位移模板档位(预测值,见文件头)。
    int gridSize = 0;

    bool hasAny() const {
        for (int e = 0; e < kEdges; ++e) {
            if (nodeCount[e] > 0) return true;
        }
        return false;
    }
    int filledEdges() const {
        int n = 0;
        for (int e = 0; e < kEdges; ++e) n += nodeCount[e] > 0 ? 1 : 0;
        return n;
    }
};

/// cell 键:schemeId interned 句柄哈希掺 4bit + z/x/y 打包。z≤27 层内
/// x,y < 2^27,本引擎 z≤18 富余。**单一事实源**:TileEdgeSnapResolver 的渲染集
/// 索引与 edgeLutTables 的查表必须同一打包 —— 两处各写一遍必然有一遍写错。
inline uint64_t terrainEdgeCellKey(const TileKey& k) {
    const uint64_t scheme = std::hash<SchemeId>{}(k.schemeId) & 0xFull;
    return (scheme << 60) |
           (static_cast<uint64_t>(static_cast<uint32_t>(k.z) & 0x3F) << 54) |
           (static_cast<uint64_t>(static_cast<uint32_t>(k.x) & 0x7FFFFFF)
            << 27) |
           static_cast<uint64_t>(static_cast<uint32_t>(k.y) & 0x7FFFFFF);
}

/// 本帧吸附瓦片的表集合(键 = terrainEdgeCellKey(占屏瓦片 key))。生存期挂在
/// TilePlan 上:非复用帧由 refresher 在 resolve 后整体重建;Strict reuse 帧
/// 原样沿用(纯数据,最坏是差值旧一代,无悬垂)。
using TerrainEdgeLutTableMap = std::unordered_map<uint64_t, TerrainEdgeLutTable>;

} // namespace earth_engine
