#pragma once

#include "RenderDevice.h"
#include "VirtualTexturePage.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace earth_engine {

// ============================================================
// 北极星 Phase 2b — 虚拟纹理 C 方案 PoC(骨架)
// ============================================================
//
// 目的 = **真机量 C(共享 atlas 虚拟纹理)的移动端固定开销**,回填设计 §5 的
// 「诚实账」,再拍板 §8 决策 #3(B vs C)。C 的两个固定开销未知量:
//   ① feedback buffer 回读 stall(GPU→CPU 同步屏障,移动端可能很贵)
//   ② 逐 fragment 间接采样(先采间接纹理再采物理 atlas)的固定成本
//
// **本骨架覆盖 ①**:建 feedback FBO + 物理 atlas + 间接纹理,每帧跑
// 「feedback pass → 回读(计时)→ 解码可见页 → 页表驻留 → 写间接纹理」整链,
// 把回读毫秒数报进 EarthPerf。**②(fragment 端间接采样)是下一子步**——需
// 改 terrain 片元 shader(sampler 余量已核实:terrain 是独立小 shader,GLES
// 有 unit 1-4 空、Metal 有 sampler 1-15 空,+2 装得下),骨架先不动 shader。
//
// 编排契约照搬 OffscreenPostProcess:initialize → ensureResources(每帧,尺寸
// 变惰性重建)→ tick。GPU 资源生命周期与 pass 编排共享,变化的只是内容。
//
// ⚠️ 骨架局限(诚实标注):feedback pass 目前只清背景不画真页(page-id 片元
// shader 未接),故 on-device 解码出的可见页恒为空——①的**回读 stall 计时依然
// 有效**(stall 在 GPU→CPU 同步,与画了什么无关)。真页数据待 shader 子步补。

struct VirtualTexturePocConfig {
    // feedback 缓冲相对屏幕的降采样倍数(VT feedback 惯例:亚采样,省回读带宽)。
    int feedbackDownscale = 8;
    // 物理 atlas 的页网格(列×行)。容量 = 列×行 个页槽。
    int atlasColumns = 8;
    int atlasRows = 8;
    // 单页边长(texels)。atlas 纹理 = (列×页边) × (行×页边)。
    int pageSizeTexels = 256;
    // 间接纹理边长(texels):每 texel 编码一个虚拟页当前落在哪个物理槽。
    // 骨架占位固定尺寸;真实尺寸应覆盖虚拟页网格,属 shader 子步再定。
    int indirectionSize = 128;
    // 异步回读(VT 生产形态):true → 走 PBO+fence 管线(enqueue 本帧/acquire N
    // 帧前的),量真实异步 stall;false → 同步 glReadPixels(量最坏上界)。后端不
    // 支持异步(Metal/mock)时自动回落同步。
    bool asyncReadback = true;
    // 异步取回延迟帧数:enqueue 后隔几帧再 acquire(隔够则 GPU 已完成、零 stall)。
    int readbackLatencyFrames = 2;
};

/// 一帧 tick 的测量结果(供 EarthPerf 报告与验收)。
struct VirtualTexturePocFrameStats {
    double feedbackPassMs = 0.0;  // feedback pass(begin/submit/end)CPU 侧耗时
    double readbackMs = 0.0;      // 回读 CPU stall(同步=glReadPixels;异步=acquire)
    double enqueueMs = 0.0;       // 异步 enqueue 耗时(同步模式恒 0)
    double updateMs = 0.0;        // 解码+页表+写间接纹理耗时
    int visiblePages = 0;
    int residentPages = 0;
    int newlyResident = 0;
    int evicted = 0;
    bool thrashed = false;
    bool readbackOk = false;      // 本帧取到了像素(异步下延迟帧内可能 false)
    bool async = false;           // 本帧走的是异步路径
    bool readbackPending = false; // 异步:acquire 时 GPU 未完成(需再等,非 stall)
};

class VirtualTexturePoc {
public:
    VirtualTexturePoc() = default;
    ~VirtualTexturePoc() = default;

    VirtualTexturePoc(const VirtualTexturePoc&) = delete;
    VirtualTexturePoc& operator=(const VirtualTexturePoc&) = delete;

    /// 建常驻 GPU 资源(物理 atlas + 间接纹理)与页表。失败返回 false。
    /// feedback FBO 尺寸依赖屏幕,推迟到 ensureResources。
    bool initialize(RenderDevice* device, const VirtualTexturePocConfig& config);

    /// 每帧:按当前屏幕尺寸惰性(重)建 feedback FBO,尺寸不符时销毁重建。
    /// 返回 false 表示 FBO 不可用(调用方跳过本帧 tick)。
    bool ensureResources(int surfaceWidthPixels, int surfaceHeightPixels);

    /// 跑一帧 feedback→回读→页表→间接纹理整链,产出测量。资源未就绪时
    /// 返回全 0 stats(readbackOk=false)。
    VirtualTexturePocFrameStats tick();

    /// 释放全部 GPU 资源(surface 销毁 / context lost)。
    void dispose();

    bool isReady() const {
        return device_ != nullptr && atlasTexture_ != nullptr &&
               indirectionTexture_ != nullptr;
    }
    const VirtualTexturePocFrameStats& lastStats() const { return lastStats_; }

    /// 物理 atlas 常驻字节(= 屏幕封死的固定占用,供 §5 诚实账)。
    int64_t atlasBytes() const;
    const VirtualTexturePocConfig& config() const { return config_; }

private:
    RenderDevice* device_ = nullptr;
    VirtualTexturePocConfig config_;
    std::unique_ptr<Texture> atlasTexture_;
    std::unique_ptr<Texture> indirectionTexture_;
    std::unique_ptr<Framebuffer> feedbackFbo_;
    std::unique_ptr<VtPageTable> pageTable_;
    int feedbackWidth_ = 0;
    int feedbackHeight_ = 0;
    std::vector<uint8_t> readbackBuffer_;
    // 异步回读在途票号队列(FIFO):enqueue 本帧尾,acquire 队首(隔够延迟帧)。
    std::deque<uint64_t> pendingTickets_;
    bool asyncSupported_ = false;  // 首次 enqueue 探测:后端是否真支持异步
    // 间接纹理 CPU 影像(RGBA8):页表更新逐槽写这里,再整张/局部上传 GPU。
    std::vector<uint8_t> indirectionCpu_;
    VirtualTexturePocFrameStats lastStats_;

    void writeIndirection(const std::vector<VtIndirectionUpdate>& updates);
};

}  // namespace earth_engine
