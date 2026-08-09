#pragma once

#include "../../renderer/GpuFrameTiming.h"

#include <cstdint>
#include <string>
#include <vector>

namespace earth_engine {

/// GL_EXT_disjoint_timer_query 之上的**平铺区间** GPU 计时器。
///
/// 为什么是平铺而不是嵌套:TIME_ELAPSED 查询同一时刻只能有一个在跑(嵌套是
/// GL 错误),而 EXT_disjoint_timer_query 的 TIMESTAMP 路径在多数 Adreno/Mali
/// 驱动上 GL_QUERY_COUNTER_BITS 为 0(报支持、实则无效)。与其在两条路径间做
/// 运行期分支(其中一条还是"看起来能用但恒 0"的那种),不如只支持平铺:一条
/// GPU 时间线切成互不重叠的段,total = Σ段。这也正是"哪个 pass 贵"要的答案。
///
/// 不 stall 的做法:每帧的查询对象进环形槽,回读**滞后 kFrameRing 帧**且用
/// QUERY_RESULT_AVAILABLE 非阻塞轮询。任何一帧结果没就绪就跳过,绝不等 GPU
/// —— 一旦等,测量本身就成了被测对象(VT PoC 的同步回读踩过这条)。
class GpuRegionTimerGles {
public:
    /// 回读滞后帧数。3 帧是 GPU 落后 CPU 的常见上界,4 给一帧余量。
    static constexpr int kFrameRing = 4;
    /// 每帧区间数上限。命令交错会把段数打碎(每次桶切换 = 一个查询对象),
    /// 无上限时几百个查询会把驱动的命令流本身变成开销。超限的段不计时,
    /// 只累计到 droppedRegions —— 报"我漏了 N 段"远好过悄悄给个偏小的和。
    static constexpr int kMaxRegionsPerFrame = 48;

    /// 查扩展 + 取函数指针。必须在 GL context 当前的线程调用。
    /// 返回 false = 本设备用不了(调用方按"没有计时"降级,不是错误)。
    bool initialize();
    void shutdown();
    bool available() const { return available_; }

    void beginFrame(uint64_t frameId);
    /// 开一个区间;上一个还开着的区间在此处结束。
    void beginRegion(const std::string& name);
    void endRegion();
    /// 收尾当前区间 + 采 disjoint + 非阻塞回读最老的一帧。
    void endFrame();

    bool regionOpen() const { return activeQuery_ != 0; }
    /// 最近一帧已回读完成的结果;还没有任何一帧完成时为 nullptr。
    const GpuFrameTiming* lastResult() const {
        return lastValid_ ? &last_ : nullptr;
    }

private:
    struct Sample {
        std::string name;
        unsigned int query = 0;
    };
    struct FrameSlot {
        uint64_t frameId = 0;
        bool pending = false;
        bool disjoint = false;
        int dropped = 0;
        std::vector<Sample> samples;
    };

    unsigned int acquireQuery();
    void recycle(FrameSlot& slot);
    /// 非阻塞:结果未就绪返回 false,槽保持 pending 等下一帧再试。
    bool tryResolve(FrameSlot& slot);

    bool available_ = false;
    FrameSlot slots_[kFrameRing];
    int currentSlot_ = 0;
    unsigned int activeQuery_ = 0;
    std::vector<unsigned int> freeQueries_;
    GpuFrameTiming last_;
    bool lastValid_ = false;
    /// 已开的区间数(含被上限挡掉的),用于 kMaxRegionsPerFrame 判定。
    int regionsThisFrame_ = 0;
};

}  // namespace earth_engine
