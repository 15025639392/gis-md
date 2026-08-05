#pragma once

#include <memory>

namespace earth_engine {

/// 渲染线程放置策略 —— 让宿主自建的渲染线程被调度器当「有显示截止期的任务」对待。
///
/// 为什么需要:宿主自己 new 出来的渲染线程对系统是匿名的。Android 只对自家
/// RenderThread 做特殊调度,一条裸 std::thread 会被 EAS 判成"利用率不到 50%,小核
/// 装得下" —— 但小核上同一份工作(命令数/瓦片数逐字段相同)要 ~21ms 而不是 ~8ms,
/// 必然错过 16.67ms 预算。真机实测:改前 91% 的帧落在小核簇、~90% 掉到 30fps;
/// 应用本策略后 100% 的帧落在性能核簇、锁 60fps。
///
/// 三级降级,每级都比下一级更"正确",只在上一级不可用时下探:
///   ① ADPF hint session —— 官方机制,申报目标时长 + 逐帧回报实际耗时,由系统
///      决定选核与升频。不写死核心拓扑、不与 governor 对抗、轻载仍能省电。
///   ② uclamp_min       —— ①的底层机制,给任务算力下界。
///   ③ 亲和到性能核簇    —— 兜底,写死放置。核心集合按运行时 cpufreq 拓扑判定。
///
/// SDK 不创建渲染线程,所以这套策略做成宿主可调用的 helper,而不是引擎内部行为。
/// 用法(除构造/析构外,所有调用都必须发生在渲染线程内):
///
///     RenderThreadPlacement placement;                 // 构造可在任意线程
///     // 渲染线程入口:
///     const auto status = placement.applyToCurrentThread();
///     // 每帧(不含等 vsync 的时间):
///     placement.reportActualWorkDurationMs(cpuWorkMs);
///     // 线程退出前(析构亦可):
///     placement.release();
///
/// Android 之外的平台是空实现(mode = Unavailable),调用点无需加平台分支。
class RenderThreadPlacement {
public:
    enum class Mode {
        Unavailable,              ///< 三级全不可用,未做任何放置干预
        PerformanceHint,          ///< ① ADPF hint session
        UtilClampMin,             ///< ② uclamp_min
        PerformanceCoreAffinity,  ///< ③ 亲和到性能核簇
    };

    struct Options {
        /// 每帧目标 CPU 工作时长。刻意略小于真实 vsync 周期(60Hz 取 16.6 而非
        /// 16.67),留一点余量让系统把"刚好用满"判成需要提速。
        double targetFrameMs = 16.6;
        /// 渲染线程 nice。Android 自家 RenderThread 跑在 display(-4),这里对齐
        /// 到 urgent-display(-8)。注意 nice 只影响同一 runqueue 内的时间片权重、
        /// **不参与选核** —— 单靠它救不了放置,留着只为对齐优先级语义。
        int threadNice = -8;
        /// ② 的算力下界,取值 0-1024;512 约等于"半个大核"。
        unsigned utilClampMin = 512;
    };

    struct Status {
        Mode mode = Mode::Unavailable;
        /// ③ 生效时绑定的核心数;其它模式恒为 0。
        int pinnedCoreCount = 0;
        /// 实际读回的线程 nice —— 设置可能被系统拒绝,以读回值为准。
        int threadNice = 0;
    };

    RenderThreadPlacement();
    ~RenderThreadPlacement();

    RenderThreadPlacement(const RenderThreadPlacement&) = delete;
    RenderThreadPlacement& operator=(const RenderThreadPlacement&) = delete;

    /// 在**当前线程**上应用放置策略。① 登记的是 gettid(),所以必须在渲染线程内
    /// 调用。返回实际落到哪一级,供宿主打诊断日志。
    Status applyToCurrentThread(const Options& options);
    /// 同上,用默认 Options(60Hz / urgent-display / uclampMin=512)。
    Status applyToCurrentThread() { return applyToCurrentThread(Options{}); }

    /// 每帧回报本帧实际 CPU 工作耗时;仅 ① 生效时有意义,其它模式是 no-op。
    /// **不要把等 vsync 的时间(eglSwapBuffers / present)算进来** —— 那会让系统
    /// 以为我们每帧都刚好用满预算,反而提不上频。
    void reportActualWorkDurationMs(double actualMs);

    /// 刷新率变化时更新目标时长(如 60Hz → 120Hz)。仅 ① 生效时有意义。
    void setTargetFrameMs(double targetFrameMs);

    /// 释放 ① 的 session —— 它绑的是登记时的 tid,线程退出前必须调用(析构会自动
    /// 调用)。②③ 随线程消亡自然失效,无需回滚。
    void release();

    Status status() const;

    /// **当前线程**此刻所在的 CPU 编号;平台不支持时返回 -1。
    ///
    /// 放在这里而不是独立的平台工具:选核就是本类的主题,读回实际落点是验证
    /// 放置是否生效的唯一直接证据(①②只是"申报意图",系统可以不理会)。真机
    /// 曾出现 91% 的帧落在小核簇、掉到 30fps,而所有上层指标——命令数、瓦片
    /// 数、加载状态——逐字段相同;当时唯一能分辨的就是这个数。
    ///
    /// 静态方法:调用方(引擎帧循环)不持有 RenderThreadPlacement 实例——渲染
    /// 线程由宿主创建、策略也由宿主应用,引擎只是想读一下自己跑在哪。
    ///
    /// 注意结果随时可能变(调度器可在任意时刻迁移线程),它是采样不是断言。
    static int currentCpu();

    /// 供日志用的稳定短名:"none" / "adpf" / "uclamp" / "affinity"。
    static const char* modeName(Mode mode) {
        switch (mode) {
            case Mode::PerformanceHint:         return "adpf";
            case Mode::UtilClampMin:            return "uclamp";
            case Mode::PerformanceCoreAffinity: return "affinity";
            case Mode::Unavailable:             break;
        }
        return "none";
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace earth_engine
