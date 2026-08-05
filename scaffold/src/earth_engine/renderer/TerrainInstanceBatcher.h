#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "RenderCommand.h"

namespace earth_engine {

class RenderDevice;
class Renderer;
class Buffer;

/// 地形实例化合批(方案 A / Step 3)。
///
/// 掠视宽视野 settled 帧的主导成本 = 逐瓦片 draw 的 O(N) 链(SUBMITDIAG:
/// draw 段 ~27µs/draw × 135 = 3.4-4.1ms)。同 {schemeId,z,row} 的可见地形
/// 瓦片在 P5b 后已共享同一位移模板 VBO/IBO,且 Step1/2 后高度纹理与间接
/// 纹理都在共享 texture2DArray——「同网格 + 同资源」实例化前提全部成立。
///
/// 本类在帧命令列表装配完(逐瓦片 stamped 命令已进 commands)、MVP compose
/// 之前运行:把资格瓦片按模板 VBO 身份分组,每组 ≥2 个 → 合成一条
/// glDrawElementsInstanced;每瓦片差异(相对批参考帧 frame0 的刚体帧、
/// minH/range·fade、morphFactor、高度/间接层号、clip)下沉进 per-instance
/// 属性流。draw 数 135 → 模板组数(~10-20)。
///
/// **资格闸**(任一不满足 → 留逐 draw,零回归):真实地形 + 用共享位移模板
/// (hasTerrainDisplacementFrame)+ pageStore 全 cell 驻留(mappedRaster
/// fallback 必不采样 → 批 shader 丢 mappedRaster)+ 无 water mask + 无
/// baseColor 纹理 + 不 blend。粗瓦片/fill/上采样/page-in 中的瓦片天然留逐
/// draw。批参考帧 float 化的相对量抖动 ~0.005px(见设计文档 §7),不可见。
///
/// 实例缓冲池按 batch 槽复用(grow-only,glId 稳定 → VAO 缓存不失效)。
class TerrainInstanceBatcher {
public:
    /// per-instance 记录:96B,与 kTerrainInstanceStride / 实例化 shader 属性
    /// 布局逐字节一致(loc 4-9 = 6× vec4)。rel 三行 = 相对 frame0 的刚体帧
    /// (行主序上三行,第 4 行恒 0001,shader 点积重建 world 局部位置)。
    struct InstanceRecord {
        float relRow0[4];   // rel_i 行 0
        float relRow1[4];   // rel_i 行 1
        float relRow2[4];   // rel_i 行 2
        float dispMorph[4]; // minH·fade, range·fade, morphFactor, gridN
        float clipUv[4];    // clip 窗口(x,y,w,h)
        float layers[4];    // heightLayer, indirLayer, clipEnabled, 保留
    };
    static_assert(sizeof(InstanceRecord) == 96, "matches kTerrainInstanceStride");

    /// 批内位移模板栅格边长(layers[3])是否全部一致。
    ///
    /// 提成公开纯函数是为了**可被单独证伪**:分组走 VBO 指针、判据走每实例携带的
    /// gridN,两条独立数据流;但驱动整个 assemble 需要真 GL 上下文(实例化 shader
    /// 未就绪时它直接早退),所以判据本身必须能脱离设备被测。契约的活性另由
    /// contracts 的 coverage 计数在真机上证明。
    ///
    /// 空批与单实例批恒真(没有可比对象),这是刻意的:在退化输入上报警只会训练出
    /// "这条警告可以忽略"的习惯。
    static bool batchTemplateGridIsUniform(const InstanceRecord* records,
                                           size_t count);

    /// 首个与批首档位不一致的 gridN;全一致时返回批首档位(空批返回 -1)。
    /// 只用于把违约现场写进日志 —— 首违约那一条是唯一能拿到的现场。
    static float firstMismatchedTemplateGrid(const InstanceRecord* records,
                                             size_t count);

    struct Stats {
        int eligibleCommands = 0;   // 通过资格闸的逐瓦片命令数
        int batchedCommands = 0;    // 被合并进批的命令数(= 省掉的 draw 数近似)
        int batches = 0;            // 生成的 instanced 命令数
    };

    /// 就地改写 commands:资格瓦片按模板分组,≥2 组合成 instanced 命令,
    /// 剩余(不资格 / 单例组)原样保留。device/renderer 空或无资格 → no-op。
    /// 必须在 MVP compose(SceneRenderCommandUniformUpdater)之前调用——批
    /// 命令置 hasTerrainDisplacementFrame 由 updater 算 mvp=viewProj·frame0。
    Stats assemble(RenderCommandList& commands, RenderDevice* device,
                   Renderer& renderer);

private:
    // 实例缓冲池:每 batch 槽一个 dynamic VBO,跨帧复用(容量足够则 glId 稳定,
    // VAO 缓存命中)。容量不足才重建(warmup 后不发生)。
    static constexpr int kInstanceBufferCapacity = 256;  // 每批最多实例数
    std::vector<std::unique_ptr<Buffer>> instanceBufferPool_;
    std::vector<InstanceRecord> recordScratch_;

    Buffer* acquireInstanceBuffer(int slot, const InstanceRecord* records,
                                  int count, RenderDevice* device);
};

}  // namespace earth_engine
