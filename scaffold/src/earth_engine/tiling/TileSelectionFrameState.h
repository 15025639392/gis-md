#pragma once

#include "TilePlan.h"

namespace earth_engine {

struct TileSelectionFrameState {
    bool completeRenderable = false;
    bool renderable = false;
    bool subdivisionDesired = false;
    TileSelectionState previousSelectionState =
        TileSelectionState::NotVisited;
    TileSelectionState selectionState = TileSelectionState::NotVisited;
    double screenSpaceError = 0.0;
    double priority = 0.0;
    bool inFrustum = false;
    bool cameraInside = false;
    bool ancestorMeetsSse = false;
    // 距离连续 geomorph 进度(terrain only):由本瓦片 SSE 在有效 LOD 频带
    // (maxSSE/2, maxSSE] 内的位置决定,每帧刷新,喂给 geomorphUpFactor.w。
    // 1 = 全细节(无 morph/geomorph 关);0 = 粗起点≈父面(刚从父级细化出)。
    float terrainMorphFactor = 1.0f;
    // 无缝北极星机制 B(边吸附):4 条边各 3bit 的 log2 吸附步长,打包
    // W + 8·E + 64·N + 512·S(N=v0 边,S=v1 边;0=不吸附)。由
    // TileEdgeSnapResolver 每帧从**实际渲染集**算出——邻居比本瓦片粗 k 个
    // 八度(z 差 + dense/coarse 档差)时,本瓦片该边顶点在 shader 里吸附到
    // 2^k 间距的自纹理线性插值 → T-junction 在几何上不存在(残余只剩金字塔
    // 层间重采样差 ε,由裙墙覆盖)。逐帧重算天然覆盖"邻居本帧刚换档"暂态。
    float edgeSnapPacked = 0.0f;
    // ①-1:本帧该瓦片的吸附边记录(邻居来源),供 draw 侧构建边高度差表。
    //
    // ⚠️⚠️ **按值存 + 帧号闸,两者缺一不可** —— 这里曾是裸指针指向
    // TilePlan::edgeSnapRecords 的元素,真机 SIGSEGV(RenderThread 跑约 7 分钟后
    // 在 TerrainEdgeHeightLut::build 顶部解引用垃圾指针)。两条独立的跨帧悬垂:
    //   ① 指针本身:向量每帧 clear()+push_back,容量一涨就重分配 → 旧地址失效。
    //      解析器只遍历**本帧计划内**的瓦片,上帧在计划内、本帧不在的瓦片
    //      永远等不到那次清空,于是带着野指针进 draw。→ 按值存根治。
    //   ② 记录**内容**:rec.tile / rec.neighbor[] 是裸指针,指向的瓦片可能已被
    //      TileSubtreeRemovalCoordinator 擦除。按值存对这条无效。→ 帧号闸根治。
    // 只修 ① 会留下更隐蔽的版本(记录还在、瓦片已死);只修 ② 则依赖"每帧都
    // 写 frameId"这个新的全覆盖假设。两条一起才闭合。
    //
    // 消费端一律经 edgeSnapRecordForFrame() 取,不要直接读这两个字段。
    TileEdgeSnapRecord edgeSnapRecord;
    bool edgeSnapRecordValid = false;

    /// ⚠️ 有意**不用帧号**做闸:draw 侧手边的 frameNumber 是 Tileset 自己的
    /// 计数,而解析器手边的是 frameState.frameId —— 两个不同的计数器。拿它们
    /// 比相等会恒假,LUT 就此静默失效(画面无变化、无报错,只有接边 ε 悄悄
    /// 退回改前)。改用「解析器每帧把上一帧盖过章的瓦片逐个撤章」,不需要两侧
    /// 对计数器达成一致 —— 少一个可以静默走歪的约定。
    const TileEdgeSnapRecord* validEdgeSnapRecord() const {
        return edgeSnapRecordValid ? &edgeSnapRecord : nullptr;
    }

    void updateFrameRenderability(bool complete) {
        completeRenderable = complete;
        renderable = complete;
    }

    void clearFrameRenderability() {
        completeRenderable = false;
        renderable = false;
    }

    void updateTraversalRenderability(bool traversalRenderable) {
        renderable = traversalRenderable;
    }
};

} // namespace earth_engine
