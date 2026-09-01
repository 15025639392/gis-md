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
/// (hasTerrainDisplacementFrame)+ pageStore 全 cell 驻留(directComposite
/// fallback 必不采样 → 批 shader 丢 directComposite)+ 无 water mask + 无
/// baseColor 纹理 + 不 blend。粗瓦片/fill/上采样/page-in 中的瓦片天然留逐
/// draw。批参考帧 float 化的相对量抖动 ~0.005px(见设计文档 §7),不可见。
///
/// 实例缓冲池按 batch 槽复用(grow-only,glId 稳定 → VAO 缓存不失效)。
class TerrainInstanceBatcher {
public:
    /// per-instance 记录:112B,与 kTerrainInstanceStride / 实例化 shader 属性
    /// 布局逐字节一致(loc 4-10 = 7× vec4)。rel 三行 = 相对 frame0 的刚体帧
    /// (行主序上三行,第 4 行恒 0001,shader 点积重建 world 局部位置)。
    struct InstanceRecord {
        float relRow0[4];   // rel_i 行 0
        float relRow1[4];   // rel_i 行 1
        float relRow2[4];   // rel_i 行 2
        // w 原本是页存储 gridN(几何等分)。cell 网格改成源瓦片网格后要带
        // cellsX/cellsY/texCoordSet 三个值,而逐实例只剩这一个槽(layers.w 是
        // 位移模板边长,在用),故打包进同一个 float,见 packPageCellDescriptor。
        float dispMorph[4]; // minH·fade, range·fade, morphFactor, pageCellDesc
        float clipUv[4];    // clip 窗口(x,y,w,h)
        float layers[4];    // heightLayer, indirLayer, clipEnabled, 模板 gridN
        // [瓦界对齐] 几何 UV→源格仿射的前 4 系数(单位:源瓦片)。实例化片元的
        // psUv 是共享模板几何 UV,g = c0 + u·dU + v·dV(dV 在 pageAux.zw)。
        // 标准 overlay 下 dU=(gridN,0)、dV=(0,gridN) → 退化为旧 uv*span 语义。
        float pageUv[4];    // c0.x, c0.y, dU.x, dU.y
        // 祖先寻址相位(x0/y0 mod 2^kMaxDetDepthLevels)+ zw=几何仿射 dV。
        // 单独占一个 vec4 而不是继续往 dispMorph[3] 里挤:那个 float 已经装了
        // cellsX+128·cellsY+16384·texSet(上限 122944),再乘 131072 进相位会越过
        // float32 的精确整数上限 2^24,静默丢位 —— 而丢位的表现是屏幕块状错乱,
        // 与"没生效"长得一样,查起来很贵。16B/实例换确定性,值。
        float pageAux[4];   // phaseX, phaseY, 保留, 保留
    };
    static_assert(sizeof(InstanceRecord) == 128,
                  "matches kTerrainInstanceStride");

    /// 把 (cellsX, cellsY, texCoordSet, cellZoom) 压进一个 float,逐实例只剩
    /// 一个槽。cellsX/cellsY ≤ 64(间接纹理边长上限),texCoordSet ≤ 7,
    /// cellZoom ≤ 31 → 最大 131072·31+122944 ≈ 4.19M < 2^24,float32 精确
    /// 可表示,不丢位。cellZoom 逐实例带是分级宽度的反台阶前提:批级
    /// uniform 承首实例会让批内异 zoom 瓦片的线宽错档(瓦界宽度跳变)。
    static constexpr float packPageCellDescriptor(int cellsX, int cellsY,
                                                  int texCoordSet) {
        return static_cast<float>(cellsX) +
               128.0f * static_cast<float>(cellsY) +
               16384.0f * static_cast<float>(texCoordSet);
    }

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

    /// 逐瓦片命令**没能进批**的原因。互斥且穷举 —— 一次运行就能给全部假设投票,
    /// 不必逐条改代码试(契约 BatchTemplateGridParity 的 coverage=0 就是靠这组
    /// 计数从"批为什么不成形"里分出真因的)。
    enum class RejectReason : uint8_t {
        NotGltfPrimitive = 0,   // 非 glTF 图元命令
        NotTerrainContent,      // 不是地形内容
        NotRealTerrain,         // fill 代理 / 椭球,不是真地形
        NoDisplacementFrame,    // 未走共享位移模板
        WrongVertexStride,      // 顶点步长非 32(未走紧凑地形格式)
        PageStoreOff,           // 页存储未对该命令生效
        NotFullyResident,       // 页未全 cell 驻留(会采 directComposite 回落)
        HasWaterMask,           // 带水面掩码,走别的着色路径
        HasBaseColorTexture,    // 带基色纹理
        HasTerrainFillMask,     // 每瓦片 fill page 不能复用批首纹理表
        Blended,                // 混合(淡入淡出期)
        Count
    };
    static const char* rejectReasonName(RejectReason reason);

    struct Stats {
        int eligibleCommands = 0;   // 通过资格闸的逐瓦片命令数
        int batchedCommands = 0;    // 被合并进批的命令数(= 省掉的 draw 数近似)
        int batches = 0;            // 生成的 instanced 命令数

        // ---- 判因:batches=0 时用来分辨"卡在哪一步" ----
        // 三步链路各有独立信号,不必猜:
        //   shaderReady=0        → 实例化 shader 未就绪,assemble 整个早退
        //   eligibleCommands=0   → 没有命令通过资格闸,看 rejects 哪一项最大
        //   groups>0 但 batches=0 → 有资格命令但每组不足 2 个(分组太碎)
        bool shaderReady = false;
        int totalCommands = 0;
        int groups = 0;             // 资格命令按模板 VBO 聚出的组数
        int singletonGroups = 0;    // 只有 1 个成员、因此放弃合批的组数
        int oversizeGroups = 0;     // 超实例容量、整组回退逐 draw 的组数
        int rejects[static_cast<size_t>(RejectReason::Count)] = {};
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
