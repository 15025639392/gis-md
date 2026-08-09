#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace earth_engine {

/// 一帧的 GPU 区间计时结果(逐 pass / 逐命令桶)。
///
/// 区间是**平铺**的,不嵌套:同一时刻只有一个区间在计时,开一个新区间即关掉
/// 上一个。这既是 GL_EXT_disjoint_timer_query 的硬约束(TIME_ELAPSED 查询不可
/// 嵌套),也正好是我们要的语义 —— 一条 GPU 时间线切成若干段,total 是各段之和
/// 而非"包住各段的外层"。
///
/// ⚠️ 读数的三条边界(不写在这里,后面必然有人拿它下错结论):
///  1. **移动端是 TBDR**。一个 pass 内部按 bin 重排执行,段与段的边界不是真正的
///     时间分割点;段内 vertex/binning 与 fragment 两阶段的成本会互相渗透。段
///     粒度只在"pass 之间"和"数量级差异"上可信,不要拿它做 5% 级的比较。
///  2. **MSAA resolve / compositor 不在任何区间内**。resolve 发生在 pass 结束
///     与 eglSwapBuffers,不在我们的命令流里。sum(regions) 显著小于 kgsl
///     gpubusy 推算的每帧 GPU 时间时,差值的第一嫌疑就是它。
///  3. **disjoint=true 的帧整帧作废**。GPU 被抢占或调频会让计时器失去意义,
///     此时读数不是"稍微不准",是无意义。
struct GpuFrameTiming {
    struct Region {
        std::string name;
        double ms = 0.0;
        int count = 0;  ///< 本帧该名字出现的段数(命令交错会让同名段出现多次)
    };

    uint64_t frameId = 0;
    std::vector<Region> regions;
    double totalMs = 0.0;
    /// 本帧计时窗口内发生过 GPU disjoint(抢占/调频)→ 读数已丢弃,不进 regions。
    bool disjoint = false;
    /// 超过每帧区间上限后未计时的段数。>0 表示 regions 不完整(命令交错太碎)。
    int droppedRegions = 0;
};

}  // namespace earth_engine
