#pragma once

#include "../core/async/WorkLedger.h"

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../core/math/OrientedBoundingBox.h"
#include "../core/math/Rectangle.h"
#include "../threading/CancellationToken.h"
#include "../tiling/RasterOverlayProjection.h"
#include "../tiling/TileKey.h"

namespace earth_engine {

class RenderDevice;
class Texture;
class ActivatedRasterOverlay;
class RasterOverlayTileProvider;
class TileScheme;
struct DecodedImage;
struct RenderCommand;
struct SelectorView;
struct TilesetTile;

/// 共享层池的等尺寸块 LRU 分配器(北极星合成方案「多瓦片页表」核心,§14.1)。
///
/// texture2DArray 的 [0, blockCount*blockLayers) 层被切成 `blockCount` 个等尺寸块
/// (每块 `blockLayers` = gridN² 层)。每个 capped 地形瓦片认领**一整块连续层**
/// (§决策① uniform-grid:layerBase + row*gridN + col 闭式寻址,无 indirection 纹理);
/// 跨瓦片零共享(#3 实测 mappings≈uniquePages),故块粒度 LRU 足够、天然无碎片。
///
/// **决策① 已拍板**:等尺寸块 + 块粒度 LRU + uniform-grid 公式(不建 indirection
/// 纹理)。可变 depth(Step B 屏幕驱动)时块尺寸不一 → 届时升级为 buddy 分配器,
/// 接口不变。
///
/// 纯 CPU、可 host 单测(与 RenderDevice/TilesetTile 解耦)。
class TerrainPageLayerPool {
public:
    /// blockCount 个块,每块 blockLayers 层。重配清空所有驻留。
    void configure(int blockCount, int blockLayers);

    /// key 已驻留 → 返回其 layerBase(否则 -1)。不改动 recency。
    int layerBaseFor(uint64_t key) const;

    /// 认领 key 的块并返回 layerBase:
    ///  - 已驻留:touch 到 frameId,返回其 base(*outEvicted=0)。
    ///  - 有空块:占用,返回 base(*outEvicted=0)。
    ///  - 无空块:淘汰 lastFrame < frameId 的最久块(*outEvicted=被淘汰 key),返回其 base。
    ///  - 所有块本帧都被 touch(lastFrame==frameId)→ 返回 -1(调用方回落 mappedRaster)。
    int acquire(uint64_t key, uint64_t frameId, uint64_t* outEvicted);

    /// 若 key 驻留则 touch 到 frameId(祖先回退保活:被当帧显示的祖先页不该被淘汰)。
    /// **不分配、不淘汰**——key 不驻留则 no-op(区别于 acquire)。
    void touch(uint64_t key, uint64_t frameId);

    /// 显式移除 key(析构/失效)。key 不在则 no-op。
    void release(uint64_t key);

    int blockLayers() const { return blockLayers_; }
    int blockCount() const { return static_cast<int>(slots_.size()); }
    int residentCount() const { return static_cast<int>(keyToSlot_.size()); }

private:
    struct Slot {
        bool used = false;
        uint64_t key = 0;
        uint64_t lastFrame = 0;
    };
    std::vector<Slot> slots_;
    int blockLayers_ = 1;
    std::unordered_map<uint64_t, int> keyToSlot_;  // key → slot index
};

/// 单页的多源合成状态机(C-1,纯 CPU、可 host 单测,与 RenderDevice/provider 解耦)。
///
/// 页存储的一层承载「**有序** overlay 列表按序 alphaOver 后的合成结果」——与
/// mappedRaster 那条路径的多 overlay 语义对齐。C-1 之前页存储只认 `overlays.front()`,
/// 靠后的 overlay 被静默丢弃,这正是「注册成功 + 瓦片 200 + 绑进 draw + 屏幕全无」
/// 那类问题的根(两条合成路径语义不一致)。
///
/// **严格按源序合成**:alphaOver 不可交换,源 i 必须等 0..i-1 全部合成后才能进 accum,
/// 乱序早到的源暂存 stash。每推进一步允许上传一次 → 部分到达先点亮(底图先亮,不必
/// 等最慢的源),避免「页到达时间 = 最慢那个源」。
///
/// 未完成页的常驻内存上界 = (1 + 乱序源数) × side²×4;`releaseBuffers()` 在全部到齐
/// 并上传后释放 —— 故稳态零额外内存。单源(sourceCount==1)时逐字节等价于 C-1 之前
/// (首源直接拷贝,不走 alphaOver)。
class PageSourceAssembler {
public:
    /// sourceCount ≤ 0 或 side ≤ 0 → 停用(accept 恒 false)。重配清空已有进度。
    void configure(int sourceCount, int sideTexels);

    /// 收下第 sourceIndex 源的 side²×4 RGBA8(非预乘直通 alpha)。
    /// 返回 true = accum 有新内容需上传。重复源 / 越界 / 空指针 → false(幂等,
    /// 防重复 fetch 到达把同一源写两遍)。
    bool accept(int sourceIndex, const uint8_t* rgba);

    bool hasTexels() const { return composited_ > 0; }
    bool complete() const {
        return sourceCount_ > 0 && composited_ >= sourceCount_;
    }
    int compositedCount() const { return composited_; }

    /// 合成缓冲(releaseBuffers 后为空;hasTexels/complete 不受影响)。
    const std::vector<uint8_t>& texels() const { return accum_; }

    /// 释放 accum/stash(调用方上传完 complete 页后调)。进度计数保留。
    void releaseBuffers();

private:
    int sourceCount_ = 0;
    int side_ = 0;
    int composited_ = 0;  // 已按序合成的源数 = 下一个待合成的源号
    std::vector<uint8_t> accum_;
    std::vector<std::vector<uint8_t>> stash_;  // 乱序早到的源(按源号索引)
};


/// 北极星合成方案「稀疏页存储」(门③ Step B2b + §14.1)。
///
/// 拥有一张共享 `texture2DArray`(§13,每层一页,天然消灭页缝),**按页粒度** LRU
/// (blockLayers=1,一页一层)驻留屏幕界定的可见影像页。determination(updateVisiblePages)
/// 每帧:遍历可见 capped 真实地形瓦片 → 屏幕驱动源 zoom → 枚举 gridN×gridN mercator 子
/// 瓦片 → 视锥剔除 → 逐可见页 pool.acquire(得 layer)+ kick 单页 fetch → 按当前 resident
/// 重建该瓦片的稀疏间接纹理(cell resident→RG=layer/A=255,miss→A=0)。片元经间接纹理
/// 单次 NEAREST fetch 定位 layer + 用 A 作 alphaOver factor → capped z12 瓦片显示屏幕界定
/// 的 z17 高清真实影像(crisp),内存有界(只驻留可见页)。
///
/// **决策② 共存/分层 override**:mappedRaster 对所有瓦片继续算(祖先影像 fallback);
/// 页存储按 **cell 粒度** 接管——resident cell A=1 覆盖,未 fetch/未到/视锥外 cell A=0
/// 保留 mappedRaster。page-in 延迟期该 cell 显祖先(糊但有)不出洞 = 优雅降级(§12.5#4)。
/// 非真实地形瓦片逐字节走现状,零回归。
///
/// **LRU 自愈无悬垂**:间接纹理每帧重建 → 淘汰页的 cell 自动因 pages_ 查不到而变 miss;
/// 淘汰时 erasePageEntry cancel fetch,在途到达经 drain 校验(pages_ 存在 + layer 匹配)丢弃。
class ThreadPool;

class TerrainPageStore {
public:
    struct Config {
        int pageSizeTexels = 256;    // 每层边长(标准 XYZ 影像瓦片 256)
        // B2b 稀疏页存储:array 层数 = LRU 页容量(每页一层,blockLayers=1)。
        // 512×256²×4 ≈ 128MB VRAM 上限;实际按 LRU 只驻留屏幕可见 ~125-185 页。
        int maxPages = 512;
        int maxUploadsPerFrame = 8;  // 每帧上传层数上限(涓流,勿拖动期冻结)
        /// 合成(重采样+按序 alphaOver)下放的 worker 池。真机测得 compose 是
        /// tick 的支配成本且单帧尖刺 33-37ms(2026-08-07 三段插桩),下放后渲染
        /// 线程只剩上传+叠画。null = 就地同步合成(host 测试确定性;行为与
        /// 下放前逐字节一致,只是同帧完成)。
        ThreadPool* composeWorkers = nullptr;

        /// ==== 刀2 路网 SDF 场"第二平面"(可选)====
        /// 非空时页存储维护场"第二平面"(D2 线段纹素,RGBA8,编码见
        /// LineFieldRasterizer.h)。回调契约:缓冲尺寸恒为 pageSizeTexels²×4;
        /// **回调必到**(取消/失败给全 0=空哨兵,失败安全);可在任意线程
        /// 回调。页存储对生产者(RoadFieldSource)零依赖,host 测试注入 fake。
        using RoadFieldRequestFn = std::function<void(
            const TileKey& pageTileKey, CancellationToken,
            std::function<void(std::vector<uint8_t>)>)>;
        RoadFieldRequestFn roadFieldRequest;
        /// 场解算的线色(RGBA,非预乘)——经 applyToTerrainCommand 进
        /// gltfUniforms,FS smoothstep 后 mix。样式快照,换样式换 Config。
        std::array<float, 4> roadFieldColor{0.96f, 0.96f, 0.94f, 0.86f};
        /// 分级宽度 ramp (z0, halfPx0, z1, halfPx1):FS 在局部 zoom 上线性
        /// 插值出线半宽(设备px),两端 clamp。zoom 基准=影像页 zoom(见
        /// PageStoreSamplingGLSL.h 的标定注释)。默认≈0.6→1.8 CSS px@dpr3.5。
        std::array<float, 4> roadFieldWidthRamp{12.0f, 1.05f, 16.0f, 3.15f};
        /// 步3 场平面 overzoom 解耦:场页 zoom 封顶。语义 = **烘焙输出不再
        /// 随 zoom 变化的档位** = max(场数据 maxZoom, 样式最后一个 zoom 分级
        /// 档)——取小了会把更深档才放开的要素(如 z≥15 catch-all 的末梢路)
        /// 整体滤掉(真机踩过:封 14 远山小径全灭),取大了徒增重烘。场页
        /// key = 影像页 key 的 z-封顶祖先,SDF 像素解算放大优雅(纹素下限
        /// 兜底)。拉近超封顶后场零重烘零上传、换代瞬态消失。
        int roadFieldMaxZoom = 15;
        /// 场页独立 LRU 容量。实测默认视角稳态驻留 ~25 页,64 层留 2.5×
        /// 余量;64×256²×4B(D2 RGBA8)= 16MB 封顶。
        int maxFieldPages = 64;
    };

    TerrainPageStore() = default;
    ~TerrainPageStore();

    TerrainPageStore(const TerrainPageStore&) = delete;
    TerrainPageStore& operator=(const TerrainPageStore&) = delete;

    /// 创建共享 array 纹理(maxPages 层,每页一层)。失败返回 false(调用方短路)。
    bool initialize(RenderDevice* device, const Config& config);

    bool isReady() const { return arrayTexture_ != nullptr; }

    /// 每帧(渲染线程,determination 之后、render 之前):推进帧号、排空已到达影像
    /// (限 maxUploadsPerFrame)灌对应页 layer 并置 uploaded。fetch 由 determination
    /// 在页首次命中时 kick(见 updateVisiblePages)。
    void tick();

    // ==================== 门② 屏幕可见影像页 determination(Step B2b)====================
    // 遍历本帧可见 capped 真实地形瓦片,对每个瓦片选屏幕合适影像 zoom、枚举其 gridN×gridN
    // mercator 子瓦片、视锥剔除 → **驱动**:逐可见页 pool.acquire(得 layer)+ 首次命中
    // kick fetch,按当前 resident 重建该瓦片稀疏间接纹理(见 updateVisiblePages)。保留
    // uniquePages/zRange 插桩(节流 log)验页数(真机应 ≈ 近景 125 / 地平线 185)。

    /// 给定瓦片层级与屏幕合适源 zoom → 子瓦片网格边长 gridN。
    /// sourceZoom ≤ tileZ → 1(不细分);否则 1 << (sourceZoom - tileZ)。纯函数,可单测。
    static int subtileGridN(int tileZ, int sourceZoom);

    /// 枚举 capped 瓦片在 sourceZoom 的 mercator 对齐子瓦片 key(z=sourceZoom,
    /// x=tileKey.x*gridN+dx, y=tileKey.y*gridN+dy)。schemeId 沿用 tileKey(几何/影像
    /// 同 XYZ web-mercator 分块,§前序已证 aligned)。纯函数,可单测(勿用 lat 均分)。
    static void enumerateSubtileKeys(const TileKey& tileKey, int sourceZoom,
                                     std::vector<TileKey>& out);

    /// 一个几何瓦片在**影像源瓦片空间**中的落位。
    ///
    /// 页存储原本把几何瓦片网格当影像瓦片网格用(`enumerateSubtileKeys` 的
    /// `x = tileKey.x*gridN+dx` 就是这个假设的写照)。标准 WebMercator 底图下二者
    /// 恰好重合,所以一直没暴露;GCJ-02 底图的源瓦片以偏移后的坐标编址,重合被打破
    /// —— 几何瓦片的覆盖范围落在源网格的**非整数**位置上。
    ///
    /// 这里把 cell 网格重新定义成**源瓦片网格**:每个 cell 仍恰好是一张源瓦片
    /// (页内容逐字节不变、compose 不需要重采样),错位改由 origin/span 承载。
    ///
    /// 单位是**源瓦片**:片元的 `t = origin + uv*span` → `cell=floor(t)`、
    /// `sampleUv=t-cell`。标准 overlay 退化为 `origin=(0,0)`、`span=(gridN,gridN)`,
    /// 表达式与改造前逐字符相同 = 零回归判据(见 degenerate 断言)。
    struct SourceTilePlacement {
        int x0 = 0;          ///< 覆盖范围最小源瓦片 x
        int y0 = 0;          ///< 覆盖范围最小源瓦片 y(NW:y 随南增)
        int cellsX = 1;      ///< 覆盖范围宽(源瓦片数)
        int cellsY = 1;      ///< 覆盖范围高(源瓦片数)
        double originU = 0.0;  ///< 瓦片 UV 原点在覆盖范围内的位置(单位:源瓦片)
        double originV = 0.0;
        double spanU = 1.0;    ///< 瓦片 UV 跨度(单位:源瓦片)
        double spanV = 1.0;

        /// 是否退化成「几何格 == 源格」——标准 overlay 必须为 true,
        /// 否则说明改造在不该生效的地方生效了。
        bool isDegenerate(int gridN) const {
            return cellsX == gridN && cellsY == gridN &&
                   originU == 0.0 && originV == 0.0 &&
                   spanU == static_cast<double>(gridN) &&
                   spanV == static_cast<double>(gridN);
        }
    };

    /// 求 `detailsRect`(几何瓦片在 overlay 采样空间的矩形,即逐顶点 UV 归一化所用
    /// 的那个矩形)在 `sourceZoom` 源瓦片网格中的落位。
    ///
    /// @param scheme      影像源的分块方案(决定源瓦片编址)
    /// @param detailsRect 瓦片的 overlay 采样空间矩形。**必须**取自
    ///                    `RasterOverlayDetails`,不要就地重算 —— details 可能来自
    ///                    模型真实包围(见 TileRasterOverlayDetailsGenerator 的注释),
    ///                    与瓦片声明矩形不等,重算会让 UV 与 cell 网格错开。
    /// @param projection  overlay 生效投影(决定 detailsRect 与源经纬的换算)
    /// @param sourceZoom  源瓦片层级
    /// @param gridN       几何等分数,仅用于退化断言
    static SourceTilePlacement placeTileInSourceGrid(
        const TileScheme& scheme,
        const Rectangle& detailsRect,
        RasterOverlayProjection projection,
        int sourceZoom,
        int gridN);

    /// [瓦界对齐] 求瓦片**几何** UV(scheme 矩形边到边 0..1,NW 原点)到源格
    /// 连续坐标的仿射(见 TileIndir::geomAffine 注释)。角点经
    /// projectWorldPositionForRasterOverlay 逐点投影——与逐顶点 texcoord 生成
    /// 同一投影入口,单一事实源。out = {c0.x,c0.y,dU.x,dU.y,dV.x,dV.y},
    /// 相对 (baseX, baseY)。
    static void computeGeomAffine(
        const TileScheme& scheme,
        RasterOverlayProjection projection,
        const TileKey& key,
        int sourceZoom,
        int baseX,
        int baseY,
        float out[6]);

    /// 对本帧可见瓦片跑门② determination + 插桩(见类顶注释)。overlay 为空 /
    /// provider 为空 / 无可见瓦片 → no-op。在 Engine tick() 之前、每帧调一次。
    ///
    /// C-1:`providers` 是**有序** overlay 列表的 provider(与 mappedRaster 同序),
    /// 每页按该序 alphaOver 合成成一层。分块/zoom/最大级由 providers[0](底图)决定
    /// —— 它是与几何对齐的那一个,靠后的源只是往同一 key 的页上叠。空表 → no-op。
    void updateVisiblePages(const SelectorView& view,
                            const std::vector<TilesetTile*>& visibleTiles,
                            const std::vector<RasterOverlayTileProvider*>& providers,
                            double terrainMaxScreenSpaceError);

    // 诊断:上一次 determination 的唯一可见页数(单测/日志)。
    int lastVisiblePageCount() const { return lastVisiblePageCount_; }

    /// 停滞多少帧之后,一个未完成的页不再算"在途"。
    ///
    /// 判据必须有**终止态**:页可以长期停在 partial(某个源在该 key 上根本没有
    /// 数据 —— `partialPages` 本就是被当作稳态统计的),而"任何一页未完成"这种
    /// 写法在那种页可见期间恒真,于是渲染循环永远停不下来。失效方向是良性的
    /// (不冻屏、只是白烧),但白烧正是这套 gating 要消灭的东西 —— 一个没有终止
    /// 条件的判据等于把开关焊死在"开"。
    /// 与 pan 惯性那个 bug 同源:判据没有终止条件,而消费侧有。
    static constexpr uint64_t kStalledPageFrames = 180;  // ~3s @60fps

    /// 是否还有页在等 fetch / 合成 / 上传(**近期仍在推进的**)。帧级按需渲染
    /// 据此判「还不能停帧」—— 影像是异步到达的,停帧就没人跑 drainReadyUploads,
    /// 到货永远灌不进 array,表现为该片区永久停在粗页且没有任何报错。
    ///
    /// 停滞超过 kStalledPageFrames 的页不计入:它不会因为"再画一帧"而变化,
    /// 真到货时 fetch 回调会把页推进、下一次 determination 自然再置进度。
    /// 页存储的在途登记进 WorkLedger,**Pumped**:它必须在渲染帧里被推进
    /// (drainReadyUploads 只在帧里跑),停帧 = 到货永远灌不进 array。与网络
    /// 那类"自己会走完"的 Landing 语义相反,混为一谈就等于没做这个区分。
    /// 整店一张票(不是逐页):gating 只关心"要不要继续出帧",不关心几页。
    void syncWorkTicket() {
        const bool busy = hasWorkInFlight();
        if (busy && !workTicket_.valid()) {
            workTicket_ = WorkLedger::shared().acquire(
                WorkLedger::Kind::Pumped, "terrainPageUpload");
        } else if (!busy && workTicket_.valid()) {
            workTicket_.release();
        }
    }

    bool hasWorkInFlight() const {
        for (const auto& entry : pages_) {
            const PageEntry& pe = entry.second;
            if (pageCountsAsInFlight(pe.uploadComplete(), frameId_,
                                     pe.lastProgressFrame)) {
                return true;
            }
        }
        for (const auto& entry : fieldPages_) {
            if (pageCountsAsInFlight(entry.second.uploaded, frameId_,
                                     entry.second.lastProgressFrame)) {
                return true;
            }
        }
        return false;
    }

    /// 单页的在途判定。提成公开纯函数是为了**可被单独证伪** —— 驱动整个页存储
    /// 需要真 GL 上下文,而这条规则一旦没有终止态,症状是"永远停不下来白烧电",
    /// 在任何截图和帧率读数里都看不出来。
    static bool pageCountsAsInFlight(bool uploadComplete,
                                     uint64_t frameId,
                                     uint64_t lastProgressFrame) {
        if (uploadComplete) return false;
        // 无符号相减:lastProgressFrame 永远 ≤ frameId(同一渲染线程写),
        // 但 surface 重建会把 frameId_ 归零 —— 那时旧页也已随存储一起销毁。
        if (frameId < lastProgressFrame) return true;  // 防御:视作刚有进度
        return (frameId - lastProgressFrame) <= kStalledPageFrames;
    }

    /// 在 applyPerFrameCommandState 里对每个 terrain 命令调用(**无相机,只 bind**):
    /// 若该瓦片本帧 determination 建了间接纹理(TileIndir)→ 绑 array slot20 + 间接纹理
    /// slot21 + 写 pageStoreParams(enabled=1,gridN);否则不动(mappedRaster fallback)。
    /// determination 已按 cell 粒度编好 resident/miss,此处仅 bind。
    /// C-1:源列表只由 determination 刷新(单一事实源),此处不再兜底捕获 provider。
    void applyToTerrainCommand(RenderCommand& cmd, const TilesetTile& tile);

    Texture* arrayTexture() const { return arrayTexture_.get(); }

    // 合批 Step 2:间接纹理共享 array 的层边长与层数(层边长 = 最大 gridN =
    // 1<<kMaxDetDepthLevels;层数按峰值可见瓦片 ~185 留余量,64²×4B×256≈4.2MB)。
    static constexpr int kIndirSideTexels = 64;
    static constexpr int kIndirArrayLayers = 256;

    /// 稀疏虚拟纹理间接纹理的 RGBA8 层编码:R=layer&0xFF、G=(layer>>8)&0xFF、
    /// **B=depth(per-cell 渐变 LOD,§16.3)**、A=resident?255:0。引擎不支持整数纹理,
    /// 故用 RGBA8 承载 16 位 layer 索引(容 ≤ 65535 页)+ 深度 d(≤ kMaxDetDepthLevels,
    /// 单 fetch 无需第二张 RG16 纹理)。**depth = Z-Za**:该 cell 采样的粗祖先页相对本瓦片
    /// 屏幕界定 max zoom Z 下降的级数(0=精页/现状,>0=粗页,片元用 span=2^d 定位子区)。
    /// **A 通道**:0=miss(保留 mappedRaster)、255=影像 ready。历史三态的
    /// 128(影像在/场 pending)自步3 场解耦后不再产出——场 ready 归独立的
    /// 场间接纹理(fieldIndirArrayTexture_),两平面门控互不牵连;fieldReady
    /// 形参保留兼容编码函数签名(调用点恒 true)。
    /// out 需 ≥4 字节;depth clamp 到 [0, kMaxDetDepthLevels]。
    static void encodeLayerRGBA8(int layer, bool resident, bool fieldReady,
                                 int depth, uint8_t out[4]);
    /// 与片元 shader 解码逐位一致:floor(r*255+0.5)+floor(g*255+0.5)*256 = R+G*256。
    /// 供 host round-trip 单测证明「编 layer → RGBA8 → 解码回 layer」链路正确。
    static int decodeLayerRGBA8(const uint8_t in[4]);
    /// B 通道深度解码(镜像片元 floor(b*255+0.5))。供 host round-trip 单测。
    static int decodeDepthRGBA8(const uint8_t in[4]);

    /// packKey 的逆(schemeId 不入 key,还原为缺省)。供叠画钩子拿页的 z/x/y。
    /// 与 packKey 必须 round-trip —— 还原错了会去取错误的源瓦片,画面表现是
    /// 「路网整体错位」而不是报错。
    static TileKey unpackKey(uint64_t packed);
    /// packKey 的单测入口(打包本身是私有实现细节,但 round-trip 必须可测)。
    static uint64_t packKeyForTest(const TileKey& key) { return packKey(key); }

    /// C-1b:把某源到达的影像重采样成本页的 side²×4 RGBA8。
    ///
    /// 页 zoom 由屏幕(与底图上限)驱动,常深于标注/矢量类源自己的 maxZoom;那些源
    /// 的 fetch key 被钳到各自上限的祖先页,`depth`/`subX`/`subY` 记下页在祖先内
    /// 的格位,此处按 scale-bias 取该子矩形双线性放大 —— 与 mappedRaster 那条路
    /// 逐瓦片挑祖先同语义。**不钳会让这些源恒 404 → 永不到达 → 卡住 assembler 的
    /// 按序游标 → 该源在页内彻底消失**(真机踩过:矢量路网整片没了)。
    ///
    /// depth=0 且 image 尺寸 == side 时逐字节等价于直拷(0.5 偏移相消,双线性权重 0)。
    /// 通道数 1/3/4 都收(单通道铺灰度);空图/非法尺寸 → out 全零。纯函数,可 host 单测。
    static void resamplePageSource(const DecodedImage& image, int depth,
                                   int subX, int subY, int side,
                                   std::vector<uint8_t>& out);

    // --- 诊断(单测/日志用)---
    int residentPageCount() const { return pool_.residentCount(); }
    int uploadedLayerTotal() const { return uploadedLayerTotal_; }

private:
    struct PendingInbox;  // 定义在 .cpp:worker 回调安全投递解码影像

    /// 页粒度账本(B2b):每个屏幕可见影像页 (z,x,y) 一层 + 异步 fetch 状态。
    /// pool.acquire 得 layer → 建 PageEntry → **逐源** kick fetch → 到达按源序合成上传。
    /// 淘汰/换租时 cancel 全部在途 fetch(到达经 drain 校验丢弃)。
    ///
    /// C-1:`uploaded` 拆成 assembler 的两个状态 —— `hasTexels()`(至少一源已合成上传,
    /// determination 据此判 cell resident)与 `complete()`(全源到齐,可释放缓冲)。
    struct PageComposeState;  // 定义在 .cpp:worker 侧合成状态(mutex 序列化)

    struct PageEntry {
        int layer = -1;
        /// 合成状态归 worker 侧(shared_ptr:worker 任务可比页存储活得久;
        /// 淘汰置 cancelled,迟到结果经 drain 校验丢弃)。
        std::shared_ptr<PageComposeState> compose;
        std::vector<CancellationToken> fetchTokens;  // 每源一个
        /// **已上传**的合成进度镜像(渲染线程独占,drainReadyUploads 推进)。
        /// determination 判 resident 必须跟"已上传"走 —— 合成下 worker 后
        /// "已合成"与"已上传"分离,跟合成走会让间接纹理采到未写入的层。
        int uploadedSources = 0;
        int totalSources = 0;
        /// 最近一次"上传进度前进"的帧号(建页时初始化为当帧)。
        /// **只服务于帧级按需渲染的在途判定**,不参与 resident/合成任何逻辑。
        uint64_t lastProgressFrame = 0;
        bool uploadedTexels() const { return uploadedSources > 0; }
        bool uploadComplete() const {
            return totalSources > 0 && uploadedSources >= totalSources;
        }
    };

    /// 步3 场页账本(独立于影像 PageEntry;key = z-封顶场页 key)。
    /// 在途判定纳入 hasWorkInFlight(场没到不停帧,停滞兜底同影像页)。
    struct FieldPageEntry {
        int layer = -1;
        CancellationToken token;
        bool uploaded = false;
        uint64_t lastProgressFrame = 0;
    };

    /// 每个屏幕可见 capped 瓦片的稀疏间接纹理(gridN×gridN RGBA8)。
    /// **每帧在 determination 里按当前 resident 页重建**:cell 命中 resident+uploaded
    /// 页 → 编 RG=layer、A=255;否则(视锥外/未 fetch/未到)→ A=0(miss)。
    ///
    /// 合批 Step 2:per-tile 纹理 → 共享 texture2DArray(固定 kIndirSideTexels²
    /// 每层,texel 写左上 gridN² 区,片元 texelFetch 整数寻址不受空余区影响)。
    /// 层由 indirPool_ LRU 认领;可见瓦片每帧重建即 touch,当帧层不被淘汰;层
    /// 被夺走(离屏久驻)→ layer 置 -1 → applyToTerrainCommand 跳过 → 回落
    /// mappedRaster(优雅降级,无 stale 采样——绑定每帧从本表读,无常驻引用)。
    /// tile 不再可见 → sweep 清除 + 释放层。
    struct TileIndir {
        int layer = -1;
        int gridN = 1;
        // cell 网格落位(源瓦片单位)+ 片元该用的 texcoord 集。determination 算好,
        // applyToTerrainCommand 原样搬进 uniform —— 命令构建侧不得自行推导,
        // 否则就又出现两个「几何格 == 源格」的独立假设。
        SourceTilePlacement placement;
        int texCoordSet = 0;
        // cell 网格 zoom(determination 的 p.zoom 快照)——FS 分级宽度的局部
        // zoom 基准,经 roadFieldParams.y / 合批 pageCellDesc 下发。
        int cellZoom = 0;
        bool fullyResident = false;  // 全 cell 高清页驻留 = 合批资格(丢 mappedRaster)
        uint64_t lastFrame = 0;  // determination 里 touch;sweep 清非本帧可见瓦片
        // [瓦界对齐] 几何 UV → 源格的逐瓦仿射 {c0.x,c0.y,dU.x,dU.y,dV.x,dV.y}
        // (单位=源瓦片,相对 placement.x0/y0;c0=NW 角)。instanced 管线的 psUv
        // 是共享模板的**几何** UV(边到边 0..1),不能喂给按「details 逐顶点
        // texcoord」标定的 origin/span——GCJ 下二者差出该瓦包围矩形的翘曲量,
        // 表现为瓦界影像/矢量错缝(实测 ~30m)。改为四角逐点投影拟合仿射(交叉
        // 项保留,扭曲项 ~2cm 可弃):相邻瓦共享角点值相同 → 瓦界按构造连续;
        // 残差=瓦内二阶弯曲(~2m,亚纹素级)。标准投影下 dU/dV 轴对齐,逐片元
        // 表达式与旧 origin/span 等价。逐瓦(非合批)管线不消费此仿射。
        float geomAffine[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    };

    static uint64_t packKey(const TileKey& key);

    /// kick 单页的**全部源** fetch(worker 回调把解码影像投进 inbox,带
    /// pageKey+layer+源号)。每源一个 token,存进 entry.fetchTokens 供淘汰时 cancel。
    /// 步3:kick 单个场页的生产(roadFieldRequest → fieldInbox_)。
    void kickFieldFetch(const TileKey& fieldKey, uint64_t fieldPacked,
                        int layer, FieldPageEntry& entry);
    /// 步3:淘汰场页(cancel 在途生产 + 移除账本;fieldLayerKey_ 不清,
    /// 同 key 重建凭它跳烘)。
    void eraseFieldEntry(uint64_t fieldPacked);
    void kickPageFetches(const TileKey& pageTileKey, uint64_t pageKey, int layer,
                         PageEntry& entry);
    void drainInbox();
    /// 取走 worker 已合成的页快照,按预算上传 + 叠画(渲染线程)。
    void drainReadyUploads();
    void erasePageEntry(uint64_t pageKey);

    RenderDevice* device_ = nullptr;
    Config config_{};
    std::unique_ptr<Texture> arrayTexture_;
    /// 刀2/步3 场"第二平面":R8 独立 array(maxFieldPages 层,**不再**与影像
    /// 层号对应)。场页 key = 影像页 key 的 z-封顶祖先(roadFieldMaxZoom),
    /// 独立 LRU(fieldPool_)+ 独立场间接纹理定位。仅 roadFieldRequest 非空
    /// 时创建;创建失败只禁用场平面,不拖垮影像页。
    std::unique_ptr<Texture> fieldArrayTexture_;
    /// 场间接纹理共享 array:与 indirArrayTexture_ 同尺寸同层数,**复用同一
    /// tile 的 ind.layer 层号**(每瓦一层,RG=场层 B=场页深度 A=ready)。
    std::unique_ptr<Texture> fieldIndirArrayTexture_;
    struct RoadFieldInbox;  // 定义在 .cpp:场 R8 快照投递箱(存活于回调)
    std::shared_ptr<RoadFieldInbox> fieldInbox_;
    /// 每**场层**当前已上传真场的场页 key(size=maxFieldPages)。同 key 场页
    /// 淘汰后重建:层仍装本页真场 → 复用不重烘不闪(churn 门控,同刀2 法)。
    std::vector<uint64_t> fieldLayerKey_;
    static constexpr uint64_t kInvalidFieldKey = ~0ull;  // z0/0/0 是合法 key,故用 MAX
    TerrainPageLayerPool fieldPool_;  // 场页独立 LRU(blockLayers=1)
    std::unordered_map<uint64_t, FieldPageEntry> fieldPages_;  // 场页账本
    std::vector<uint8_t> fieldIndirTexelsScratch_;  // 场间接纹理上传复用缓冲

    TerrainPageLayerPool pool_;
    std::unordered_map<uint64_t, PageEntry> pages_;      // pageKey → 页账本
    std::unordered_map<uint64_t, TileIndir> tileIndirs_;  // tileKey → 稀疏间接纹理
    std::unique_ptr<Texture> indirArrayTexture_;  // 合批 Step 2:间接纹理共享 array
    TerrainPageLayerPool indirPool_;              // 间接纹理层 LRU(blockLayers=1)
    WorkLedger::Ticket workTicket_;
    uint64_t frameId_ = 0;
    int uploadedLayerTotal_ = 0;

    // 220ms 归属拆分:tick 内两段窗口计时。compose=重采样+按源序合成(纯 CPU),
    // upload=updateTextureRegion。
    // 每 60 tick 打一行窗口累计 + 单帧峰值后清零 —— 分不开归属就没法回答
    // "220ms 花在哪",也没法查纯运动期是否在做无谓重合成(items 应趋 0)。
    double winComposeMs_ = 0.0;
    double winUploadMs_ = 0.0;
    double winMaxTickMs_ = 0.0;
    int winInboxItems_ = 0;
    int winFieldUploads_ = 0;  // 刀2 诊断:真场层上传数(证明第二平面在工作)
    // [V24] 场洞诊断:kept cell 中"精确档 pending 且祖先回退也无存货"的数
    // (= 该 cell 本帧线不画)。缩放期持续 >0 = 线闪的机制信号;
    // 回退命中数单列,两数并读可判"回退在扛"还是"池被踢穿"。
    // [P1] 无谓重合成的直接判据:**同一页 key 被建过一次以上**
    // (= churn 中被淘汰又要回来 → 重拉 + 重解码 + 重合成全套重来)。
    // tick60 的 items 只数"合成了几次",分不出首次与重来 —— 那正是
    // P1 一直无法判定的原因。everCreated 只存 key(8B/页),诊断可接受。
    std::unordered_set<uint64_t> everCreatedPages_;
    int winPagesCreated_ = 0;
    int winPagesRecreated_ = 0;
    int winFieldHoleCells_ = 0;
    int winFieldFallbackCells_ = 0;

    // C-1:有序源列表(providers_[0] = 底图,定分块/zoom/最大级)。每帧由
    // determination 刷新;变化时作废全部已合成页(旧页少一层或多一层都是错的)。
    std::vector<RasterOverlayTileProvider*> providers_;
    std::vector<RasterOverlayTileProvider*> detProvidersScratch_;  // 剔 null 复用
    std::shared_ptr<PendingInbox> inbox_;            // 跨线程投递箱(存活于回调)
    struct ReadyUploadInbox;  // 定义在 .cpp:worker 合成完毕的快照投递箱
    std::shared_ptr<ReadyUploadInbox> readyInbox_;   // 同上,存活于 worker 任务

    // 门② determination:子瓦片相对 capped 瓦片的最大细分深度上限
    // (屏幕驱动一般 ≤5;cap 防远景/病态 zoom 枚举爆量,gridN ≤ 1<<cap)。
    static constexpr int kMaxDetDepthLevels = 6;
    /// 祖先寻址相位的模数 = 最大 span(2^kMaxDetDepthLevels)。也是间接纹理边长
    /// 上限,两者相等不是巧合:cell 网格最宽就是这么多格。
    static constexpr int kMaxDetSpan = 1 << kMaxDetDepthLevels;
    std::unordered_set<uint64_t> visiblePagesScratch_;  // 每次 determination 复用(dedup/计数)
    std::vector<uint8_t> indirTexelsScratch_;           // 间接纹理上传复用缓冲
    // 合批资格判因:全 cell 驻留是唯一卡点,只有布尔结果时"差一点"与"根本达不到"
    // 分不开。每次 determination 重置,随 PageDet 一起输出。
    int residencyCheckedTiles_ = 0;
    int fullyResidentTiles_ = 0;
    float worstResidentRatio_ = 1.0f;
    uint64_t pageDetFrameCounter_ = 0;                  // 节流 log 用(独立于 frameId_)
    int lastVisiblePageCount_ = 0;

    // ===== determination 缓存:per-cell 几何(源矩形→OBB,相机无关)按 placement 缓存,
    // 每帧只在其上做相机相关的 frustum/SSE 分类得到 kept。相机平滑运动下 placement 稳定 →
    // OBB 跨帧复用(免每帧重算=几何 walk 大头),分类逐位等于现算故 kept 不变。驻留层
    // (pool.acquire/touch/淘汰/kick/编码)**每帧必跑**,故异步到页逐帧点亮无 stale。=====
    struct DetKeptCell {  // 缓存的「过视锥」cell(纯几何,不含驻留态)
        int dx = 0;
        int dy = 0;
        int d = 0;         // per-cell 渐变 LOD 深度(§16.3):Z-Za,pageKey/fetchKey=粗祖先
        uint64_t pageKey = 0;
        TileKey fetchKey;  // 影像 provider schemeId 的粗祖先页 key(kick fetch 用)
    };
    // [pageStore第一刀] 相机无关的 per-cell 几何(源矩形→OBB,建构含三角函数,是几何
    // walk 的大头)。只随 placement 变化重建,平滑运动下跨帧复用;分类阶段每帧在其上做
    // frustum+SSE,结果逐位等于现算 → kept 不变,无白面/漏底风险。
    struct DetCellGeom {
        int dx = 0;
        int dy = 0;
        int subX = 0;  // p.placement.x0 + dx(pageKey 的 cx=subX>>d 用)
        int subY = 0;
        OrientedBoundingBox obb;
        DetCellGeom(int dx_, int dy_, int subX_, int subY_,
                    const OrientedBoundingBox& obb_)
            : dx(dx_), dy(dy_), subX(subX_), subY(subY_), obb(obb_) {}
    };
    struct DetTileCacheEntry {
        int gridN = 1;
        // cell 范围(源瓦片网格)。与 gridN 分开存:GCJ 下 gridN 不变而范围可能
        // 整体平移,只比 gridN 会把上一格的 kept 当本格用 = 屏幕错开一整张源瓦片。
        int cellsX = 0;
        int cellsY = 0;
        int cellX0 = 0;
        int cellY0 = 0;
        uint64_t lastFrame = 0;  // 访问帧;sweep 清非本帧可见瓦片(同 tileIndirs_)
        // [pageStore第一刀] 相机无关几何缓存(placement 不变则跨帧复用,免每帧重建 OBB)。
        std::vector<DetCellGeom> geom;
        std::vector<DetKeptCell> kept;
        // 本瓦片落在 SSE 地板以下、被降级到**最粗祖先页**兜底的远 cell 数(纯诊断)。
        //
        // 历史:此计数曾名 sseFloorCulled —— 那时地板以下的 cell 被直接剔除(A=0 回落
        // mappedRaster),故要参与合批资格判定。刀1/刀2 后 drape 面与 SDF 路网场只寄生在
        // 页上、mappedRaster 无矢量等价物,剔除 = 远景高俯角整片没水面/没路网;改为让这些
        // cell 照常走渐变路径取粗页(za 恒钳 tileZ,与本瓦近地板 cell 同页,零新增页)。
        // 现在它们进 kept、有页、参与 residentCells → 合批资格由 residentCells==kept.size()
        // 统一覆盖,**本计数不再进合批闸**,仅供真机验收判「远景补覆盖是否生效(应 >0)」。
        int coarseFarCells = 0;
    };
    struct DetTileParam {  // 本帧 per-tile 参数(签名阶段算,walk/encode 复用)
        TilesetTile* tile = nullptr;
        uint64_t tileKeyPacked = 0;
        int zoom = 0;
        int gridN = 0;
        double minH = 0.0;
        double maxH = 0.0;
        SchemeId imgSchemeId;  // 影像 provider 的 schemeId(interned handle)
        // 瓦片在**源瓦片网格**中的落位。标准 overlay 恒退化成几何等分
        // (isDegenerate),GCJ 等源网格不对齐的 overlay 才非平凡。
        SourceTilePlacement placement;
        // 片元该用哪套 texcoord 定位 cell。页存储原本硬编码 set 0 = 地形 scheme
        // 的投影;GCJ 的 UV 烘在另一套里,取错就等于这个特性没生效。
        int texCoordSet = 0;
        // 源坐标 → 世界坐标的平移量(瓦片中心处求值,弧度)。
        //
        // cell 网格搬进源网格后,`scheme.tileToRectangle(sourceKey)` 给的是**源
        // 坐标系**的矩形;直接拿它做视锥剔除/测距,等于把 cell 的位置整体挪了一个
        // δ(~500m),视锥边缘的 cell 因此被误剔 —— 真机上表现为东侧一整片空洞
        // (heading=0 时屏幕右侧,与 GCJ 偏东一致)。编址归源空间,剔除/测距归世界
        // 空间,两件事必须分开。δ 在单瓦片内变化可忽略,故每瓦片求一次即可 ——
        // 这个前提只在瓦片不跨 GCJ 边界时成立,见 crossesGcjBoundary。
        // 标准 overlay 恒为 0 → 逐字节等价于改造前。
        double worldOffsetLng = 0.0;
        double worldOffsetLat = 0.0;
        // 瓦片跨 GCJ 变换边界(境内境外各一部分)。此时 δ 在瓦片内是 ~500m 的**阶跃**
        // 而非梯度,单个 worldOffset 代表不了:境外那半边的 cell 会被错误地平移一个
        // δ,视锥边缘因此误剔一条约半 cell 宽的带。
        //
        // 误剔本身不是新问题,但它在这里会**击穿合批资格闸**:闸放行"视锥外"的 cell
        // 是因为它们不产生片元(见 fullyResident 处的三类论证),而这里的"视锥外"
        // 是假的 —— 那些 cell 会产生片元,合批后没有 mappedRaster 回落,渲成纯白面。
        //
        // 故跨界瓦片放弃视锥剔除(保守全收),让资格闸按 kept 全驻留正常挡住合批。
        // 代价是那一列瓦片多取几张页;它们只出现在中国框四条边上(帕米尔/俄远东/
        // 印尼海域/俄北),不在任何中国境内视角里。标准 overlay 恒 false。
        bool crossesGcjBoundary = false;
    };
    std::unordered_map<uint64_t, DetTileCacheEntry> detTileCache_;  // tileKey→几何缓存
    std::vector<DetTileParam> detParamsScratch_;
};

}  // namespace earth_engine
