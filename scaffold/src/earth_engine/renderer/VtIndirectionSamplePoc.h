#pragma once

#include "RenderDevice.h"

#include <cstdint>
#include <memory>

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
//   • 一屏尺寸离屏目标,跑两组全屏 fill pass:
//       baseline = descent 0(仅 1 次 atlas 采样)= B fill 的 1-sample 地板;
//       descent  = descent N(N 次依赖间接 fetch + 1 次 atlas 采样)。
//     **同一帧同一 GPU 态量两组** → 直接得「间接降相对一次平采样的倍率」,这
//     就是门①的答案(过门 = 倍率可接受、与 B fill 1.3ms 同量级;过不了 = 退
//     Option-lite:静态 base 走 B、动态层留现成 raster)。
//   • fill 在整屏 → 片元数 = 真实屏幕像素数(最坏 fill 上界,与 B fill 台对齐)。
//
// 局限(诚实标注,与 B/C PoC 对称):①间接寻址数学是**代表性成本模型**(N 次
// 依赖 fetch + 1 次 atlas 采样),非 cesium 逐字节等价的 Octree.glsl 移植——量
// 的是「依赖间接链 + atlas 采样」的 fragment 开销结构,不是最终生产 shader;
// ②GLSL only,Metal 无 shader 时 isReady=false(测量台在 Adreno);③性能是
// debug 构建但 fill 是 GPU-bound 故基本可信([[perf-measured-on-debug-build]])。

struct VtIndirectionSamplePocConfig {
    // 间接降层数 = 片元里依赖 texture fetch 的次数(取址依赖上次取回值)。
    // 0 由 baseline 固定占用;descent 组用此值。realistic:capped 粗页到目标
    // 页之间的 LOD 跨度 ~3-6。调此值可量「每多降一层多花多少」。
    int descentLevels = 4;
    // 间接纹理边长(texels)。小(页网格粒度),NEAREST 采样。
    int indirectionSize = 256;
    // 共享 atlas 边长(texels)。linear 采样(与真实影像采样一致)。
    int atlasSize = 2048;
    // 每组每帧重复的全屏 pass 数(压过计时噪声;两组各跑这么多次取每-pass 均值)。
    int passesPerTick = 8;
};

// 一帧 tick 的测量结果(供 EarthPerf 报告与拍板)。
struct VtIndirectionSampleFrameStats {
    double baselineMs = 0.0;  // 每-pass:仅 1 次 atlas 采样(= B fill 1-sample 地板)
    double descentMs = 0.0;   // 每-pass:N 次依赖间接 fetch + 1 次 atlas 采样
    double ratio = 0.0;       // descentMs / baselineMs(门①核心结论:间接降的倍率)
    int descentLevels = 0;    // 本次 descent 组的层数
    int64_t fillPixels = 0;   // 每 pass fill 的片元数(= 屏幕像素数)
    int passes = 0;           // 每组实际跑的 pass 数
    bool ready = false;
};

class VtIndirectionSamplePoc {
public:
    VtIndirectionSamplePoc() = default;
    ~VtIndirectionSamplePoc() = default;

    VtIndirectionSamplePoc(const VtIndirectionSamplePoc&) = delete;
    VtIndirectionSamplePoc& operator=(const VtIndirectionSamplePoc&) = delete;

    /// 建间接纹理 + atlas + 两组 shader(baseline/descent)。GLSL only:Metal
    /// 或 shader 建不出 → isReady()=false,tick 退化空转。失败返回 false。
    bool initialize(RenderDevice* device,
                    const VtIndirectionSamplePocConfig& config);
    /// 惰性(重)建一屏尺寸离屏 fill 目标;尺寸变则重建。返回 false 表示不可用。
    bool ensureResources(int surfaceWidthPixels, int surfaceHeightPixels);
    /// 跑一帧:passesPerTick 个 baseline pass + passesPerTick 个 descent pass,
    /// 分别计时,产出每-pass 均值与倍率。资源未就绪返回全 0 stats。
    VtIndirectionSampleFrameStats tick();
    void dispose();

    bool isReady() const {
        return device_ != nullptr && baselineShader_ != nullptr &&
               descentShader_ != nullptr && fillFbo_ != nullptr;
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
    std::unique_ptr<ShaderProgram> descentShader_;   // #define DESCENT N
    std::unique_ptr<Framebuffer> fillFbo_;           // 一屏尺寸
    int fillWidth_ = 0;
    int fillHeight_ = 0;
    VtIndirectionSampleFrameStats lastStats_;
};

}  // namespace earth_engine
