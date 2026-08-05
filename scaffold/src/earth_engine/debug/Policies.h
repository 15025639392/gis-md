#pragma once

#include <cstdint>

namespace earth_engine {
namespace policy {

/// 策略生效率 —— 三层信号里的中间层。
///
/// 分工(与另外两层的区别是**性质**不同,不是粒度不同):
///   架构 = 静态/结构性质 → 编译期 static_assert + CI(BackendWindingContract、
///          DepthConvention、GltfUniformBlock)
///   执行 = 点性质、布尔        → 运行时契约(Contracts.h),默认全绿,违约即错
///   策略 = **分布性质、区间**  → 本文件。默认不是"绿",而是"落在预期区间内"
///
/// 为什么必须单独一层:合批资格闸事实上不可达、合批长期空转,四条契约没有一条能
/// 发现它 —— 因为每一条**单点**判断都是成立的,错的是**整体比率**(合批率恒 0)。
/// 那次是靠契约的 coverage=0 意外捞到的,而 coverage 本是为"契约自己是否还活着"
/// 设计的。同类问题再来一次(LRU 淘汰失效、上传预算恒撞上限、回落率悄悄涨到
/// 90%)今天照样发现不了。
///
/// **区间必须带出处**。一个没有理由的阈值,在第一次误报时会被调宽,第二次再调宽,
/// 然后就永远不会再报了 —— 比不设更糟,因为它看起来还在工作。所以 Expectation
/// 强制带 rationale,写不出依据的策略就先别加。
///
/// 分母为 0 = 本窗口没有机会(没有可合批的命令 / 没有可见瓦片),**不参与判定**。
/// 与契约 coverage 的 disabled 同理:「没机会生效」和「生效了但不达标」是两回事,
/// 混在一起会让报表在冷启动/空场景下疯狂误报。
enum class Id : uint8_t {
    /// 合批率 = 进批命令数 / 有资格命令数。
    BatchFormation,
    /// 页驻留率 = 全 cell 驻留(=有合批资格)的瓦片数 / 参与判定的瓦片数。
    PageResidency,
    Count
};

const char* name(Id id);

/// 期望区间(闭区间)+ 它的依据。
struct Expectation {
    double lo;
    double hi;
    /// 为什么是这个区间。会打进越界告警 —— 看到告警的人得能判断是系统坏了还是
    /// 这个区间当初就定错了,没有依据就只能盲目调宽。
    const char* rationale;
    /// 越界时该找谁(与契约的 owners 同理:报表是要拿去派活的)。
    const char* owner;
};
Expectation expectation(Id id);

/// 累计一次观测。分子/分母在窗口内累加,窗口末算比率。
///
/// 例:合批率 observe(BatchFormation, stats.batchedCommands,
/// stats.eligibleCommands) —— 逐帧调,窗口内自动聚合。逐帧比率在负载抖动下噪声
/// 极大,单帧值不具可比性(同 EnvSnap 的 cpuMs 只报窗口 avg/max)。
void observe(Id id, int numerator, int denominator);

/// 周期报表(tag="Policy")。**总是打** —— 与契约的 coverage 行同理:全在区间内
/// 时它正是唯一能证明策略还在按预期运行的东西。越界才升 Warning 并逐条点名。
void logReport(uint64_t frameId);

/// 窗口内累计的分子/分母(测试用)。
uint64_t windowNumerator(Id id);
uint64_t windowDenominator(Id id);

}  // namespace policy
}  // namespace earth_engine
