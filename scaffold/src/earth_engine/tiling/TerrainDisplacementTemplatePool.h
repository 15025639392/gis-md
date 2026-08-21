#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "../core/math/Rectangle.h"
#include "../renderer/TerrainPageStore.h"  // TerrainPageLayerPool(层 LRU)
#include "TileKey.h"

namespace earth_engine {

class RenderDevice;
class Buffer;
class Texture;
class ShaderProgram;
class Framebuffer;
struct DecodedHeightmap;

// 把 (gridSize+1)² 栅格节点烘成 RGBA8 texel:R/G = 16bit 归一化高度
// (t = (h-minHeight)/heightRange),B/A = 切空间法线 xy(nz>0 恒成立,
// shader 重建)。纯数据变换,不碰 device —— 抽成自由函数以便 host 直测
// 边界法线的跨瓦一致性(见 test_terrain_edge_normal_seam)。
// bounds 只用于把 uv 方向的高度差换算成米/米坡度。
std::vector<uint8_t> bakeTerrainHeightNormalTexels(
    const DecodedHeightmap& heightmap, const Rectangle& bounds, int gridSize,
    float minHeight, float heightRange);

// 共享模板固定栅格单元数（n=65=2^6+1，GE 嵌套栅格约定；与 grid64 一致）。
// 模板独立于瓦片原始网格密度——所有地形瓦片用同一密度共享模板，UV 均匀。
inline constexpr int kTerrainDisplacementGridSize = 64;

// ============================================================
// 自适应几何密度(解 65×65 钉死)
// ============================================================
// 背景:整条高度链原本锁死在 64 格 —— z12 瓦片地面宽 8510m / 64 = **每顶点
// 133m**,而源数据 514px = 16.6m/px,8 倍高程细节解码后留在 CPU 从未上 GPU。
// 详见 docs/issues/terrain-visual-maturity-gap-2026-08-02.md §1.1。
//
// **为什么不能全局抬密度**:真机实测峰值可见 103 片(camH 67km)。全局 64→128
// 即 103×2×128² = 3.4M 三角/帧,手机不可行(现状 844k)。
//
// **档位由屏幕空间误差驱动**,与 TerrainPageStore 选影像页 LOD 同一模式
// (见 TerrainPageStore.cpp:230 的 log2(tileSse/threshold))。关键性质:正常
// 细化的瓦片 SSE ≈ 阈值(它想细就细了),只有**几何 cap 在 DEM native max LOD、
// 想细化却细化不了**的瓦片 SSE 会远超阈值 —— 那批正是该加密的,且数量少
// (钉死场景实测 maxTileSse=675 vs 阈值 16)。于是总三角数由屏幕面积封顶,
// 不随可见瓦片数线性增长。
//
// 只设两档而非连续分档:每多一档就多一个高度纹理 array(层边长是 array 的硬
// 约束),而"正常细化 / 被 cap"本身就是二分,两档已覆盖。
inline constexpr int kTerrainDenseGridSize = 256;

// 升档阈值:瓦片几何误差投到屏幕 ≥ 这么多像素时升 dense 档。
//
// 判据用**绝对像素**而非"maximumScreenSpaceError 的倍数",两个理由:
// ① SSE 本身就是像素量纲,绝对阈值是直接可解释的("这块地的几何误差已占 64px");
// ② maximumScreenSpaceError 是用户可调的性能旋钮——若有人为了省电把它调到 64,
//    绝不该连带把几何密度翻四倍(倍数判据会,绝对判据不会)。
// 代价:与 TerrainPageStore 的倍数判据不再是同一个表达式,但两者目标不同
//    (那边是选影像页 LOD,跟随细化阈值才对齐清晰度)。
inline constexpr double kTerrainDenseGridSseThresholdPixels = 64.0;
// 降档阈值低于升档阈值 = 迟滞带。缺了它,SSE 在阈值附近抖动的瓦片会逐帧
// 升降档,而换档要重建常驻 draw 命令 + 重烘高度纹理(257² 上传)——每帧一次
// 就是可见卡顿。0.75 给出 25% 的死区,相机缓慢推进时只换一次。
inline constexpr double kTerrainDenseGridSseReleasePixels = 48.0;

// **单一事实源**:draw 侧(模板 + 高度纹理 acquire)与 CPU 侧渲染网格一致采样
// (DecodedHeightmapSampler::sampleHeightRenderGrid,矢量贴地用)必须用同一函数。
// 两侧不一致会让贴地矢量浮起或陷进地面(P3 已踩过一次)。
// currentGridSize = 该瓦片当前所在档(0 = 尚未定档),用于迟滞。
inline int terrainGridSizeForSse(double tileSse, int currentGridSize = 0) {
    const bool wasDense = currentGridSize >= kTerrainDenseGridSize;
    const double threshold = wasDense ? kTerrainDenseGridSseReleasePixels
                                      : kTerrainDenseGridSseThresholdPixels;
    return tileSse >= threshold ? kTerrainDenseGridSize
                                : kTerrainDisplacementGridSize;
}

// 档位**单一决策点**读法。档位每帧只在 update 期决策一次(refresher 带迟滞
// 写进 selectionFrameState.displacementGridSize),draw/接缝 resolver/边高度
// LUT/错位探针一律经本函数读那份决策 —— 此前四处消费者各自从 SSE 重推一遍
// (有的带迟滞有的不带),迟滞带内(SSE∈[48,64) 且曾 dense)判断互相打架:
// 吸附步长按低估 2 级的八度算 → 该吸不吸,探针同源同错。决定只做一次,
// 分歧在结构上消失。decided<=0(该瓦片本帧未被 refresher 盖章:测试假体/
// 病理态)回落无迟滞预测 = 旧首建行为。
// ⚠️ dense 池触顶/每帧预算耗尽的 draw 期回落**不回写决策**(那发生在画的
// 瞬间,决策预测不了):接受该帧降级,下一帧失效重建自愈,predM2 计数观测。
inline int decidedOrPredictGridSize(int decidedGridSize, double tileSse) {
    return decidedGridSize > 0 ? decidedGridSize
                               : terrainGridSizeForSse(tileSse);
}

// 北极星 Phase 2c:地形位移随 LOD 连续衰减到平（GE 式「从太空看是光滑椭球，
// 靠近才长出起伏」）。粗/远瓦片（低 z）真实 relief 被粗网格 faceting 成钻石
// 尖刺 + skirt 墙外露,本就不可分辨,压平;fade→0 完全跳过位移路径(平椭球),
// fade→1 全起伏。**单一事实源**:draw 侧(GltfDrawCommandBuilder 模板 swap)与
// prepare 侧(P5b 跳过废弃 per-tile VBO 的判据)必须用同一函数,两侧判据不一致
// 会导致「建了但不画」或「不建又要画」。
inline constexpr double kTerrainReliefFadeZLo = 6.0;  // z≤6 全压平
inline constexpr double kTerrainReliefFadeZHi = 9.0;  // z≥9 全起伏

inline float terrainReliefFade(int z) {
    const double t = std::clamp(
        (static_cast<double>(z) - kTerrainReliefFadeZLo) /
            (kTerrainReliefFadeZHi - kTerrainReliefFadeZLo),
        0.0, 1.0);
    return static_cast<float>(t * t * (3.0 - 2.0 * t));  // smoothstep
}

// 北极星 Phase 2c（地形 GPU 位移）：共享位移模板的 GPU 缓冲池。
//
// 同 {schemeId, z, row(y), gridSize} 的瓦片共享一份模板 VBO/IBO——模板顶点在
// 瓦片中心 ENU 局部帧、逐列不变（见 content/TerrainDisplacementTemplate），故
// 整行只上传一次，令地形 VBO 字节从「随可见瓦片数线性」压到「每 {LOD,row} 一
// 份」（北极星 §5 有界闸门）。每瓦片的经纬度落位由 per-tile 刚体帧承担（在
// GltfDrawCommandBuilder 算 enuToEcef(center) 写进 RenderCommand），不进本池。
//
// 池由 Engine 惰性持有（terrainGpuDisplacement flag 开启时）；draw 层经
// Renderer::terrainDisplacementPool() 非空即启用（仿 TerrainPageStore 门控）。
// 模板顶点打包成与现有路径逐字节一致的 32B TerrainGpuVertex（heightDelta=0，
// Stage A 零起伏；Stage B 由高度纹理在 shader 里位移）。
class TerrainDisplacementTemplatePool {
public:
    struct TemplateBuffers {
        Buffer* vertexBuffer = nullptr;
        Buffer* indexBuffer = nullptr;
        int indexCount = 0;
        int vertexCount = 0;
    };

    void initialize(RenderDevice* device) { device_ = device; }
    bool ready() const { return device_ != nullptr; }

    // 取（必要时建+上传）key 瓦片所在 {LOD,row} 的共享模板。用 bounds 生成
    // （列无关，任一代表列即可），按 {schemeId,z,y,gridSize} 缓存复用。
    // device 未初始化或 gridSize<1 返回 nullptr。
    const TemplateBuffers* acquire(const TileKey& key, const Rectangle& bounds,
                                   int gridSize);

    // Stage B:per-tile 高度数据(gridN+1 方 RGBA8,RG 打包 16bit 归一化高度,
    // NEAREST)。shader 顶点级 texelFetch 取回、按 (minHeight,heightRange) 反量化
    // → pos = 面点 + 法线·h。
    //
    // 合批 Step 1:存储从「每瓦片一张 2D 纹理(无界增长)」改为**共享
    // texture2DArray**(每瓦片一层,固定 kHeightArrayLayers 层,层 LRU 复用
    // TerrainPageLayerPool)——实例化批内高度纹理经实例 layer id 寻址的前提;
    // 逐 draw 路径同步迁移(u_terrainLayers.x),不留双存储形态。
    //
    // 层淘汰一致性:每层带 epoch(重分配自增)。常驻 draw 命令记录其
    // (layer, epoch),draw 侧每帧用 heightLayerCurrent() 校验,失配 →
    // invalidate 命令缓存 → rebuild 重新 acquire(P5b 自愈模式)。可见瓦片
    // 每帧 touchHeightTexture 保活,当帧被 touch 的层绝不被淘汰(池语义),
    // 故只要容量 ≥ 峰值可见瓦片数(实测 ~185 << 256),淘汰只落在离屏瓦片。
    struct HeightTexture {
        Texture* texture = nullptr;  // 共享 array 纹理
        float minHeight = 0.0f;
        float heightRange = 1.0f;  // maxHeight − minHeight(下限保护 >0)
        int gridSize = 0;
        int layer = -1;            // 本瓦片高度数据所在层
        uint32_t epoch = 0;        // 该层分配代;层被重分配后旧 epoch 失效
    };

    // 无缝北极星 ①-1:每层在 (gridN+1) 方高度数据**之下**多开 4 行,存边吸附
    // 的邻居高度表(每边一行,行内第 j 个纹素 = 该边第 j 个吸附节点上「粗邻居
    // 实际渲染出的高度 − 本瓦片该纹素高度」)。
    //
    // 为什么塞进自己这张纹理而不是新建一张:LUT 每边最多 gridN/2+1 个节点
    // (coarse 33、dense 129),恒 ≤ 层宽 gridN+1 —— 放得下。于是 shader 侧
    // 零新增 sampler、实例流零改动(hLayer 本来就到得了),batcher 的「同批同
    // 表」不变量也不必动。代价仅显存 65²→65×69、257²→257×261,合计 +0.47MB。
    //
    // ⚠️ 存的是**差值**而非绝对高度:绝对高度要跟着每瓦片的 (minH,range) 反
    // 量化,而 LUT 的量程与本瓦片高度的量程不是一回事,共用会静默截断。差值
    // 走下面的固定量程,零传输且失效时天然退化(delta=0 = 改前的自纹理吸附)。
    static constexpr int kEdgeLutRows = 4;
    // 差值编码量程 ±kEdgeLutDeltaRangeMeters。16bit / 4096m = 步长 0.0625m,
    // 优于 Terrain-RGB 的 0.1m 数据步长(否则修掉 ε 又引入同量级新误差)。
    // 上界取 2048m 是因为实测 fadeDiffer 群体的错位峰值到过 1044m —— 只覆盖
    // ε(几十米)会让跨 fade 档的边静默截断。
    static constexpr float kEdgeLutDeltaRangeMeters = 2048.0f;
    static constexpr int heightLayerRows(int gridSize) {
        return gridSize + 1 + kEdgeLutRows;
    }
    /// 差值 ↔ 16bit 归一化。与 shader 侧 `eeSampleTerrainHeight` 的 RG 反量化
    /// 同一编码,只是量程换成固定的 ±kEdgeLutDeltaRangeMeters。
    /// ⚠️ 差值 0 落在量程**中点**(q=32768),不是 0 —— 全零字节会被解成 −2048m。
    static uint16_t encodeEdgeLutDelta(float deltaMeters) {
        const float r = kEdgeLutDeltaRangeMeters;
        const float t = (std::clamp(deltaMeters, -r, r) + r) / (2.0f * r);
        return static_cast<uint16_t>(t * 65535.0f + 0.5f);
    }
    static float decodeEdgeLutDelta(uint16_t q) {
        const float r = kEdgeLutDeltaRangeMeters;
        return (static_cast<float>(q) / 65535.0f) * 2.0f * r - r;
    }

    /// 写入某瓦片本帧的边 LUT 行(①-1)。bytes 为 kEdgeLutRows 行 × (gridSize+1)
    /// 个 RGBA8 纹素,RG = encodeEdgeLutDelta 的高低字节。
    /// 该瓦片当前不在该档驻留时返回 false(调用方据此清 lutValid 位)。
    bool updateEdgeLutRows(const TileKey& key, int gridSize,
                           const uint8_t* bytes);
    // GLES3.0 GL_MAX_ARRAY_TEXTURE_LAYERS 规范下限 256(Adreno 实测 2048),
    // 按规范下限定容;峰值可见 ~103 层留有余量。65²×4B×256 ≈ 4.3MB。
    static constexpr int kHeightArrayLayers = 256;
    // dense 档层数按"被 cap 的近景瓦片"定容,远小于总可见数(钉死场景 3,
    // 掠视下也只有近处那批)。257²×4B×48 ≈ 12.7MB —— 若给满 256 层要 67.6MB,
    // 手机上不可接受(tile cache 本身已压到 192MB)。触顶时 LRU 淘汰,
    // 淘汰者本帧回落 coarse 档(acquire 返回 nullptr → draw 侧降级),不丢瓦片。
    static constexpr int kDenseHeightArrayLayers = 48;

    static constexpr int layersForGridSize(int gridSize) {
        return gridSize >= kTerrainDenseGridSize ? kDenseHeightArrayLayers
                                                 : kHeightArrayLayers;
    }

    // bounds:瓦片地理范围(弧度),仅法线烘焙用 —— 把 uv 方向的高度差换算成
    // 米/米坡度需要瓦片东西/南北向的真实地面尺寸。高度本身不需要它。
    const HeightTexture* acquireHeightTexture(const TileKey& key,
                                              const DecodedHeightmap& heightmap,
                                              const Rectangle& bounds,
                                              int gridSize,
                                              uint64_t frameId);

    // 可见瓦片每帧保活(recency + 当帧免淘汰)。key 未驻留则 no-op。
    // gridSize 必须与该瓦片本帧 acquire 用的档位一致(档位各有独立 LRU)。
    void touchHeightTexture(const TileKey& key, int gridSize, uint64_t frameId);

    // 常驻命令缓存的 staleness 校验:该 (gridSize, layer, epoch) 是否仍指向当初
    // 的瓦片。档位也参与校验 —— 瓦片换档后旧命令必须失效重建(层号在另一个
    // array 里,不校验档位会拿错 array 的层)。
    bool heightLayerCurrent(int gridSize, int layer, uint32_t epoch) const {
        const HeightArray* a = findHeightArray(gridSize);
        return a && layer >= 0 &&
               static_cast<size_t>(layer) < a->layerEpochs.size() &&
               a->layerEpochs[static_cast<size_t>(layer)] == epoch;
    }

    size_t residentTemplateCount() const { return cache_.size(); }
    // 已上传模板 VBO 总字节（§5 有界性观测：应随可见 {LOD,row} 数封顶）。
    size_t totalVertexBytes() const { return totalVertexBytes_; }

    // B 方案:GPU 烘焙开关。on 时 acquireHeightTexture 不做 CPU 烘焙,而是登记
    // 一个 bake 请求;flushHeightBakes() 在渲染线程(build 之后、depth prepass
    // 之前)统一以 GPU RTT pass 填层。off = 原 CPU 烘焙路径(默认,零行为变化)。
    void setGpuHeightBakeEnabled(bool enabled) { gpuBakeEnabled_ = enabled; }
    bool gpuHeightBakeEnabled() const { return gpuBakeEnabled_; }
    // 消费 pending bake 请求:逐请求把源高度打包纹理传 GPU + 一遍片元 pass 渲进
    // 目标层。须在渲染线程、任何主/prepass pass 打开之前调(pass 不可嵌套)。
    void flushHeightBakes();
    // H-S4:消费本帧登记的边 LUT 上传。updateEdgeLutRows 只入池(字节变更检测
    // 照旧),本函数在 build 之后、submit 之前调用,按 array 把待传层拼成一段
    // 连续内存一次灌入(depth=层数),把「每瓦每帧一次小上传」摊成每 array
    // 每帧一次 —— 运动期 frameState 6.8-14.6ms 的 LUT 上传 burst 根因。
    void flushEdgeLutUploads();

private:
    struct Entry {
        std::unique_ptr<Buffer> vertexBuffer;
        TemplateBuffers view;  // indexBuffer 指向 sharedIndexBuffers_ 里的共享份
        // 建这份模板时用的地理跨度。共享的前提是「同 {z,row} 的瓦片跨度相同」,
        // 而模板由**第一个来要的瓦片**的 bounds 决定并永久缓存 —— 若该前提被
        // 破坏,整行从此都用错尺寸的几何,且不会自愈。存下来就能当场比对。
        double builtLatSpan = 0.0;
        double builtLonSpan = 0.0;
        // 建模方的 schemeId(interned handle,O(1) 指针比较)。T-P5:若某调用点
        // 用 A 的 key 配 B 的 bounds,或两套切片方案被 intern 成同一 handle,
        // 命中比对立刻暴露「谁和谁撞了同一个模板键」—— 打印双方字符串。
        SchemeId builtSchemeId;
    };

    // 按 gridSize 共享的索引缓冲(内容纯拓扑,与瓦片位置无关)。生命周期与池
    // 同长,故 Entry 里存裸指针视图是安全的(模板 entry 不可能活得比池久)。
    struct SharedIndexBuffer {
        std::unique_ptr<Buffer> buffer;
        int indexCount = 0;
    };
    // ⚠️ 键必须含**地理跨度**,不能只有 {schemeId,z,row,gridSize}。
    // 2026-08-16 真机取证:曾有跨度不同的瓦片落到同一键上(经度倍率精确 2.000
    // = rootTilesX 相差一倍的两套切片方案),后来者拿到先来者的模板 → 几何只
    // 铺一半、四周露背景;且模板永久缓存不自愈 → 整行一直错、复现具粘滞性。
    // 把跨度并进键,这类共用在结构上不再可能(代价:真有多套跨度时多存几份)。
    static uint64_t cacheKey(const TileKey& key, int gridSize,
                             double latSpan, double lonSpan);
    static uint64_t heightCacheKey(const TileKey& key);  // 逐瓦片(含列)

    // 每个密度档一套独立存储:array 层边长是硬约束(同 array 各层必须等尺寸),
    // 故 coarse(65²)与 dense(257²)不能共用一个 array。层 LRU / epoch / 驻留索引
    // 随之各自独立 —— 这也让"换档"自然表现为在另一档重新 acquire。
    struct HeightArray {
        std::unique_ptr<Texture> texture;
        int gridSize = 0;
        TerrainPageLayerPool layerPool;          // 层 LRU(blockLayers=1)
        std::vector<uint32_t> layerEpochs;       // 每层分配代
        /// 每层最后上传的边 LUT 字节(2026-08-21 H-B1):updateEdgeLutRows 按
        /// 内容变更检测,字节相同则跳过 GPU 上传 —— 108 瓦视野惯性期每帧
        /// 108 次小纹理上传(frameState 6.8-11.4ms)的根因。层被重分配
        /// (epoch 变)时清空,照常上传。
        std::vector<std::vector<uint8_t>> layerEdgeLutBytes;
        std::unordered_map<uint64_t, HeightTexture> index;  // 瓦片键 → 驻留视图
    };

public:
    // 黑带排查(2026-08-15):acquireHeightTexture 的三条 nullptr 出口各自计数。
    // 三条都会让调用方走 draw 侧降级 —— 若降级路径有缺陷,黑带就出在这里。
    // 现象是「一旦出现就容易复现」= 粘滞,故 resident/capacity 一并暴露:池饱和
    // 后 layerFull 会持续发生,这是粘滞的直接候选解释。
    struct HeightFrameStats {
        int denseBudgetRejected = 0;  // dense 逐帧限流挡回
        int layerFull = 0;            // layerPool.acquire 返回 -1(全满)
        int evicted = 0;              // 本帧淘汰的层数
        // draw 侧缓存命令的两条作废判据(命中即当帧 rebuild)。epoch 失配 =
        // 该命令引用的层已被重分配给别的瓦片 —— rebuild 之前那一瞬间,着色器
        // 拿到的是**别人的高度**。grid 失配 = 密度档变了。
        int epochMiss = 0;
        int gridMiss = 0;
        // 祖先裁剪回退的两种落地形态(黑带排查 2026-08-16):
        // remap = 命令的顶点缓冲被换成**后代模板**(深一级 → 边长减半),
        //         屏幕上只覆盖后代那一小块,祖先其余范围无几何。
        // plainClip = 只盖裁剪矩形,几何仍是祖先自己的模板。
        int surfaceClipRemap = 0;
        int surfaceClipPlain = 0;
        // 直接量症状(2026-08-16):拿到的共享模板,其地理跨度与本瓦片应有的
        // 跨度不符 —— 屏幕上就表现为「瓦片画大了/画小了,四周露背景」。
        // 不依赖任何成因假设:不符就是错,并记下第一例的 key 与倍率。
        int templateSpanMismatch = 0;
        int mismatchZ = -1;
        int mismatchX = -1;
        int mismatchY = -1;
        double mismatchLatRatio = 1.0;
        double mismatchLonRatio = 1.0;
        int coarseResident = 0;
        int coarseCapacity = 0;
        int denseResident = 0;
        int denseCapacity = 0;
    };
    const HeightFrameStats& heightFrameStats() const {
        return heightFrameStats_;
    }
    void noteHeightEpochMiss(uint64_t frameId) {
        beginHeightStatsFrame(frameId);
        ++heightFrameStats_.epochMiss;
    }
    void noteHeightGridMiss(uint64_t frameId) {
        beginHeightStatsFrame(frameId);
        ++heightFrameStats_.gridMiss;
    }
    void noteSurfaceClipRemap(uint64_t frameId) {
        beginHeightStatsFrame(frameId);
        ++heightFrameStats_.surfaceClipRemap;
    }
    void noteSurfaceClipPlain(uint64_t frameId) {
        beginHeightStatsFrame(frameId);
        ++heightFrameStats_.surfaceClipPlain;
    }

private:
    HeightFrameStats heightFrameStats_;
    uint64_t heightStatsFrameId_ = ~0ull;

    // 帧号变化即清零(与 tryConsumeDenseBudget 同一套帧边界语义)。
    void beginHeightStatsFrame(uint64_t frameId);

    // 每帧最多新建几片 dense 瓦片。release 实测单片 dense 构建
    // (高度烘焙 ~6ms + 模板 ~18ms)最坏 23.6ms;两片叠在同一帧 = 25.7ms 的
    // rebuild,超 60fps 的 16.7ms 预算。限到 1 片后单帧 rebuild ≈13ms 落回预算内。
    // 超预算的瓦片本帧回落 coarse 档(acquireHeightTexture 返回 nullptr,draw 侧
    // 已有降级重试),下一帧再升 —— 推近时逐帧升几片,不掉瓦片、不卡帧。
    static constexpr int kMaxDenseBuildsPerFrame = 1;

    // 帧号变化即重置计数。返回 false = 本帧 dense 预算已用完。
    bool tryConsumeDenseBudget(uint64_t frameId) {
        if (frameId != denseBudgetFrame_) {
            denseBudgetFrame_ = frameId;
            denseBuiltThisFrame_ = 0;
        }
        if (denseBuiltThisFrame_ >= kMaxDenseBuildsPerFrame) return false;
        ++denseBuiltThisFrame_;
        return true;
    }

    // 惰性建某档的共享高度 array。失败返回 nullptr。
    HeightArray* ensureHeightArray(int gridSize);
    const HeightArray* findHeightArray(int gridSize) const;

    RenderDevice* device_ = nullptr;
    std::unordered_map<uint64_t, Entry> cache_;
    size_t totalVertexBytes_ = 0;

    // ---- 高度纹理共享 array 存储(合批 Step 1;按密度档分组)----
    // 档数固定为 2(coarse/dense),用 map 而非固定成员是为了让 gridSize 保持
    // 唯一键、避免"哪个成员对应哪档"的隐式约定。
    std::map<int, HeightArray> heightArrays_;
    std::map<int, SharedIndexBuffer> sharedIndexBuffers_;
    uint64_t denseBudgetFrame_ = 0;
    int denseBuiltThisFrame_ = 0;

    // ---- B 方案:GPU 烘焙 ----
    // 一次 bake 请求:源已打包(hi/lo→RG8,含重叠环)按值自持(不留 heightmap
    // 指针,虽 flush 同帧同线程,按值更省心),+ 目标层 + shader uniform 全套。
    struct PendingHeightBake {
        Texture* targetTexture = nullptr;  // 目标 height array(不持有)
        int layer = 0;
        int gridSize = 0;
        std::vector<uint8_t> srcPacked;    // srcSize² RGBA8,R=码hi G=码lo
        int srcSize = 0;
        float srcInset = 0.5f;
        float quantBase = 0.0f;
        float quantStep = 0.125f;
        float minH = 0.0f;
        float range = 1.0f;
        float widthM = 1.0f;
        float heightM = 1.0f;
        float reach = 0.0f;
    };
    // H-S4:待批量上传的边 LUT 层(本帧内按层去重,后到者覆盖)。bytes 为
    // (gridSize+1)×kEdgeLutRows 的 RGBA8。
    struct PendingEdgeLutUpload {
        HeightArray* arr = nullptr;
        int layer = -1;
        std::vector<uint8_t> bytes;
    };
    // 入池(同层已在本帧待传则覆盖);供 updateEdgeLutRows 与 acquire 的
    // delta-0 初始化共用。
    void enqueueEdgeLutUpload(HeightArray* arr, int layer,
                              const uint8_t* bytes, size_t byteCount);
    bool gpuBakeEnabled_ = false;
    std::vector<PendingHeightBake> pendingBakes_;
    std::vector<PendingEdgeLutUpload> pendingEdgeLutUploads_;
    std::unique_ptr<ShaderProgram> bakeShader_;
    std::unique_ptr<Buffer> bakeQuad_;
    std::map<int, std::unique_ptr<Framebuffer>> bakeFbos_;   // 按 n=gridSize+1 定 viewport
    std::map<int, std::unique_ptr<Texture>> bakeSrcTex_;     // 按 srcSize 复用源 scratch
    bool ensureBakeResources();
};

}  // namespace earth_engine
