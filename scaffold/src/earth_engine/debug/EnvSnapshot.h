#pragma once

#include <cstdint>
#include <cstdio>

#include "PlatformLog.h"

namespace earth_engine {
namespace envsnap {

/// 运行环境快照 —— 「这次运行到底跑在什么条件下」的常驻信号。
///
/// 为什么需要:本引擎历史上代价最高的几次误判都不是代码错,是**两次运行的环境
/// 不同却被当成同构对比**——渲染线程被调度器放进小核簇(同一份工作 8ms→21ms)、
/// adb reverse 没设导致地形全 404 被当成"透洞 bug"、DEM 数据集只覆盖重庆却在
/// 境外测掠视清晰度、缓存冷热不同就跨运行比加载耗时。这类问题一律表现为"改了
/// 一版好像有效/无效",于是退化成反复 A/B,而真正的答案在一行环境读数里。
///
/// 两段刻意分开,因为腐烂速度不同:
///   - boot 段:启动打一次,是**配置事实**(数据源 URL、开关档位)。它回答
///     "这次跑的是不是我以为的那套配置"。
///   - runtime 段:每 kRuntimePeriodFrames 帧一次,是**运行时事实**(落在哪个
///     核、缓存温度、本窗口帧耗时)。它回答"这次和上次的执行条件是否可比"。
///
/// 字段准入标准:每个字段必须能追溯到一次真实的误判。想不出它杀掉过哪次错误
/// 归因,就不要加——环境行的价值在于短到每次都会被读,不在于全。
///
/// 本模块**零依赖**(只用 PlatformLog):调用方填 POD 结构,这里只负责节流与格式
/// 化。这样它不会把 HttpCache / 线程放置 / 配置结构的头文件拖进每个日志点。
namespace detail {

/// runtime 段周期。600 帧 ≈ 60fps 下 10 秒 —— 足够长到稳态几乎不产生日志量,
/// 又足够短到一次真机取证(通常 30-60 秒)能拿到多个样本看趋势。
inline constexpr uint64_t kRuntimePeriodFrames = 600;

/// 窗口内累计,给 runtime 段算均值/峰值。
/// 单帧瞬时值在 DVFS 抖动下是纯噪声(实测单次 run 帧时 ±2×),所以这里报
/// 窗口均值 + 窗口峰值:均值抗抖,峰值抓卡顿,两个数才构成可比的量。
struct FrameCpuAccumulator {
    double sumMs = 0.0;
    double maxMs = 0.0;
    uint32_t count = 0;

    void add(double ms) {
        sumMs += ms;
        if (ms > maxMs) maxMs = ms;
        ++count;
    }
    double avgMs() const { return count > 0 ? sumMs / count : 0.0; }
    void reset() {
        sumMs = 0.0;
        maxMs = 0.0;
        count = 0;
    }
};

inline FrameCpuAccumulator& frameCpuAccumulator() {
    static FrameCpuAccumulator acc;
    return acc;
}

}  // namespace detail

/// 启动段字段。全部来自场景配置 —— 这一行就是「配置声明的事实」。
struct BootFields {
    /// 渲染后端短名("GLES3" / "Metal")。iOS 上离屏后处理不支持,开关会被静默
    /// 拒绝(已有 Engine 侧 error 日志),后端名让"效果没出现"一眼可判。
    const char* backend = "?";
    /// 地形 DEM 的 URL 模板。杀掉过:adb reverse 没设(全 404 被当透洞 bug)、
    /// 数据集覆盖边界外测清晰度(FABDEM 只盖重庆 700km)。
    const char* demUrl = "";
    int demMinZoom = 0;
    int demMaxZoom = 0;
    /// 解码时**实际生效**的 nodata 哨兵个数(显式配置 + 编码隐含,取自
    /// HeightmapTerrainProvider::effectiveNoDataCount)。杀掉过:俯视黑裂缝
    /// (Terrain-RGB 的 -10000 未注册 → 数据空洞被当合法高度混进边缘双线性)。
    /// 注意别退回报配置列表长度 —— 那会在哨兵已就位时报 0,恰好在这个字段唯一
    /// 该报警的场景里给假绿灯。
    int nodataRegisteredCount = 0;
    /// 高度图边界内缩像素。0=顶点栅格源;0.5=cell-registered + 1px 重叠环。
    /// 与无缝表现直接相关,取错值即跨瓦片错位。
    float demBorderInset = 0.0f;
    /// 首个影像 overlay 的 URL 与 zoom 范围。杀掉过:裸球(高德 z0 是占位图,
    /// overlayMinimumZoom 需为 1)。
    const char* overlayUrl = "";
    int overlayMinZoom = 0;
    int overlayMaxZoom = 0;
    int overlayCount = 0;
    /// 大 flag 档位。A/B 两侧这些值必须逐字段相同,否则比的不是同一个系统。
    bool terrainGpuDisplacement = false;
    bool terrainPageStore = false;
    bool decoupleImageryFromGeometry = false;
    bool terrainFillProxy = false;
    /// 离屏后处理开关的**最终生效值**(被后端拒绝后应为 false)。
    bool fxaa = false;
    bool aerialFog = false;
};

/// 启动段:installScene 末尾打一次。URL 可能很长,故拆两行,前缀同为 "boot"。
inline void logBoot(const BootFields& f) {
    platformLog(LogLevel::Info, "EnvSnap",
                "boot backend=%s nodataReg=%d borderInset=%.2f "
                "displacement=%d pageStore=%d decouple=%d fillProxy=%d "
                "fxaa=%d fog=%d",
                f.backend,
                f.nodataRegisteredCount,
                static_cast<double>(f.demBorderInset),
                f.terrainGpuDisplacement ? 1 : 0,
                f.terrainPageStore ? 1 : 0,
                f.decoupleImageryFromGeometry ? 1 : 0,
                f.terrainFillProxy ? 1 : 0,
                f.fxaa ? 1 : 0,
                f.aerialFog ? 1 : 0);
    platformLog(LogLevel::Info, "EnvSnap",
                "boot dem=%s z=%d-%d | overlays=%d overlay0=%s z=%d-%d",
                f.demUrl[0] != '\0' ? f.demUrl : "(none)",
                f.demMinZoom, f.demMaxZoom,
                f.overlayCount,
                f.overlayUrl[0] != '\0' ? f.overlayUrl : "(none)",
                f.overlayMinZoom, f.overlayMaxZoom);
}

/// 运行段字段。全部是**每次运行都可能不同**的执行条件。
struct RuntimeFields {
    /// 本帧渲染线程实际所在的 CPU 编号(sched_getcpu;不可用为 -1)。
    /// 杀掉过:30fps 真因 = 裸渲染线程被 EAS 判成"小核装得下",同一份工作
    /// (命令数/瓦片数逐字段相同)在小核要 ~21ms。判"变慢"先读这个数。
    int cpu = -1;
    /// 本窗口可见瓦片数 / draw 数。A/B 判"变慢"前必须先确认**输入相同**——
    /// 这两个数不同就说明比的不是同一份工作量,耗时差异无意义。
    int visibleTiles = 0;
    int drawCalls = 0;
    /// HttpCache 累计命中率与查表次数。杀掉过:跨运行比加载性能时缓存温度不同
    /// (冷启动 vs 热缓存)——两次跑的不是同一件工作。
    ///
    /// 覆盖范围要记住:HttpCache 走的是**地形 DEM 瓦片 + glTF + 服务元数据**,
    /// **影像瓦片不经过它**(RasterOverlayTileProvider 自有路径)。所以这是
    /// "地形侧缓存温度",不是全局缓存温度。冷启动静止视角下 0% 是正常读数。
    int httpHitPercent = 0;
    uint64_t httpLookups = 0;
};

/// 运行段:每帧调用(内部累计帧耗时),满周期时才真正打一行。
/// @param frameId  全局帧号(FrameState::frameId)
/// @param frameCpuMs 本帧引擎 CPU 耗时,进窗口累计
inline void tickRuntime(uint64_t frameId,
                        double frameCpuMs,
                        const RuntimeFields& f) {
    detail::FrameCpuAccumulator& acc = detail::frameCpuAccumulator();
    acc.add(frameCpuMs);
    // 首帧不打:此时缓存/瓦片/线程放置都还没进入稳态,数字会误导。
    if (frameId == 0 || frameId % detail::kRuntimePeriodFrames != 0) {
        return;
    }
    platformLog(LogLevel::Info, "EnvSnap",
                "f=%llu cpu=%d cpuMs=%.2f/%.2f(avg/max,n=%u) "
                "tiles=%d draw=%d httpHit=%d%% httpReq=%llu",
                static_cast<unsigned long long>(frameId),
                f.cpu,
                acc.avgMs(), acc.maxMs, acc.count,
                f.visibleTiles,
                f.drawCalls,
                f.httpHitPercent,
                static_cast<unsigned long long>(f.httpLookups));
    acc.reset();
}

}  // namespace envsnap
}  // namespace earth_engine
