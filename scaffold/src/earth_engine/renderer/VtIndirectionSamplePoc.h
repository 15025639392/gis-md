#pragma once

#include "RenderDevice.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace earth_engine {

// ============================================================
// 北极星 Phase 2b — 合成方案「门①」原型:逐片元间接采样开销
// ============================================================
//
// 合成方案(§10/§11)= CPU 定可见页 → 上传填共享 atlas → **片元逐像素「间接降
// → 采 atlas」**。调研已证:门②(CPU 页 determination)基本已解、填 atlas 用
// 上传(cesium Megatexture)绕开渲染-into-atlas 缺口、间接纹理可移植。**唯一真
// 未知 = 门①:片元里那条「先采间接纹理定槽、再采 atlas」的链在 Adreno 上到底
// 多贵**——尤其间接降是**数据依赖的串行 texture fetch**(下一次取址依赖上一次
// 取回的值),移动 GPU 的 latency-hiding 对依赖链无能为力,这正是 VT 间接寻址
// 的成本命门。cesium 证 2D 影像(单次自顶向下降、无 ray-march)比体素便宜,但
// 「便宜」是相对体素,绝对值必须真机量。
//
// 本原型 = 纯旁路测量台(不改任何生产渲染):
//   • 建小 atlas(RGBA8)+ 扁平间接纹理(RGBA8,NEAREST,填散布的子指针)。
//   • 一屏尺寸离屏目标,**同帧同 GPU 态扫描整条深度曲线**:baseline(descent 0,
//     仅 1 次 atlas 采样 = 参照地板)+ vtSweepDepths()={1,2,3,4,6,8} 各一组
//     (n 次依赖间接 fetch + 1 次 atlas 采样)。每组末尾**强制 GPU 同步**(1×1
//     回读)后计时,减去同步地板 = 干净的每-pass GPU fill。
//     → 门①答案 = 曲线形状:realistic 浅降(1-2 层,单次页表)倍率可接受 → 过门
//     定合成方案(设计约束:间接降必须浅、优先单次 fetch 页表);深降(≥6)超线性
//     爆炸则退 Option-lite(静态 base 走 B、动态层留现成 raster)。
//   • fill 在整屏 → 片元数 = 真实屏幕像素数(最坏 fill 上界,与 B fill 台对齐)。
//   • **依赖 fetch 关键**:纯 CPU 计时只量命令编码(实测 3.44MP 报 0.02ms 不可信),
//     故强制同步纳入真实 GPU 时间;依赖链(下址依赖上次取回值)是 VT 间接寻址
//     的成本命门,latency-hiding 对它失效 → 真机实测超线性(D8=24× 悬崖)。
//
// 局限(诚实标注,与 B/C PoC 对称):①间接寻址数学是**代表性成本模型**(N 次
// 依赖 fetch + 1 次 atlas 采样),非 cesium 逐字节等价的 Octree.glsl 移植——量
// 的是「依赖间接链 + atlas 采样」的 fragment 开销结构,不是最终生产 shader;
// ②GLSL only,Metal 无 shader 时 isReady=false(测量台在 Adreno);③性能是
// debug 构建但 fill 是 GPU-bound 故基本可信([[perf-measured-on-debug-build]])。

// 一次 tick 里扫描的间接降深度集(1 次 = 经典页表;多层 = cesium octree 式自顶
// 向下降)。同一 run 量整条曲线,避免逐深度重编译-重装(「别外推,直接量」)。
// realistic:capped 粗页到目标页 LOD 跨度常 ~2-4;8 是保守上界。
inline const int* vtSweepDepths() {
    static const int kDepths[] = {1, 2, 3, 4, 6, 8};
    return kDepths;
}
constexpr int kVtSweepCount = 6;

struct VtIndirectionSamplePocConfig {
    // 间接纹理边长(texels)。小(页网格粒度),NEAREST 采样。
    int indirectionSize = 256;
    // 共享 atlas 边长(texels)。linear 采样(与真实影像采样一致)。
    int atlasSize = 2048;
    // 每组每帧重复的全屏 pass 数(压过计时噪声 + 摊薄单次强制同步开销;每组减去
    // 同步地板后取每-pass 均值)。
    int passesPerTick = 16;
};

// 一帧 tick 的测量结果(供 EarthPerf 报告与拍板)。所有 *Ms 均为**每-pass GPU
// fill**(已减去强制同步地板),单位 ms。
struct VtIndirectionSampleFrameStats {
    double baselineMs = 0.0;               // 仅 1 次 atlas 采样(无间接)= 参照地板
    double descentMs[kVtSweepCount] = {0};  // 各深度:N 依赖 fetch + 1 atlas 采样
    double syncFloorMs = 0.0;              // 单次强制同步(1×1 回读)地板,各组已减去
    int64_t fillPixels = 0;                // 每 pass fill 的片元数(= 屏幕像素数)
    int passes = 0;                        // 每组实际跑的 pass 数
    bool ready = false;
};

class VtIndirectionSamplePoc {
public:
    VtIndirectionSamplePoc() = default;
    ~VtIndirectionSamplePoc() = default;

    VtIndirectionSamplePoc(const VtIndirectionSamplePoc&) = delete;
    VtIndirectionSamplePoc& operator=(const VtIndirectionSamplePoc&) = delete;

    /// 建间接纹理 + atlas + baseline shader + kVtSweepCount 个深度 shader。GLSL
    /// only:Metal 或 shader 建不出 → isReady()=false,tick 退化空转。失败返回 false。
    bool initialize(RenderDevice* device,
                    const VtIndirectionSamplePocConfig& config);
    /// 惰性(重)建一屏尺寸离屏 fill 目标;尺寸变则重建。返回 false 表示不可用。
    bool ensureResources(int surfaceWidthPixels, int surfaceHeightPixels);
    /// 跑一帧:同步地板 + baseline + 各深度组,每组强制 GPU 同步后取每-pass GPU
    /// fill(减地板)。产出整条深度曲线。资源未就绪返回全 0 stats。
    VtIndirectionSampleFrameStats tick();
    void dispose();

    bool isReady() const {
        return device_ != nullptr && baselineShader_ != nullptr &&
               !descentShaders_.empty() && fillFbo_ != nullptr;
    }
    const VtIndirectionSampleFrameStats& lastStats() const { return lastStats_; }
    const VtIndirectionSamplePocConfig& config() const { return config_; }

private:
    // 跑 n 个全屏 fill pass(用给定 shader),返回总耗时(ms)。
    double runPasses(ShaderProgram* shader, int n);

    RenderDevice* device_ = nullptr;
    VtIndirectionSamplePocConfig config_;
    std::unique_ptr<Texture> indirectionTexture_;  // unit 0(shader 名 u_tileTexture)
    std::unique_ptr<Texture> atlasTexture_;        // unit 1(shader 名 u_depthTexture)
    std::unique_ptr<Buffer> quadBuffer_;
    std::unique_ptr<ShaderProgram> baselineShader_;  // #define DESCENT 0
    // 各扫描深度一枚 shader(#define DESCENT = vtSweepDepths()[i])。
    std::vector<std::unique_ptr<ShaderProgram>> descentShaders_;
    std::unique_ptr<Framebuffer> fillFbo_;           // 一屏尺寸
    int fillWidth_ = 0;
    int fillHeight_ = 0;
    VtIndirectionSampleFrameStats lastStats_;
};

}  // namespace earth_engine
