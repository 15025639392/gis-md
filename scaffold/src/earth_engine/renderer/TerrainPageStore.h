#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../core/math/Rectangle.h"
#include "../threading/CancellationToken.h"
#include "../tiling/TileKey.h"

namespace earth_engine {

class RenderDevice;
class Texture;
class ActivatedRasterOverlay;
class RasterOverlayTileProvider;
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

/// 页上传后的 GPU 叠画钩子(C-2c:矢量在页原生分辨率上直接画进 array 层)。
///
/// 为什么是「上传后叠画」而不是「当成一个源进 PageSourceAssembler」:assembler 走
/// CPU 合成,而矢量的整个价值就在于**不经过任何固定分辨率的中间位图** —— 一进
/// assembler 就又得先栅格化。叠画排在影像上传之后,天然就是正确的合成次序。
///
/// 次序与失效由页存储驱动,实现方不必自己管:
///  - 每次页上传后都会被调一次 → 底图重传(LRU 换租 / 祖先升级)会抹掉叠画,
///    但紧接着的这次调用又画回来,不需要额外的脏标记。
///  - 返回 false = 本页内容尚未就绪(如源瓦片还在路上)。页存储记下「未叠画」,
///    后续帧继续叫,直到成功。**别在实现里自己攒待画队列** —— 页随时可能被
///    LRU 换租,攒下来的 (页,层) 对会过期,画进别人的层里。
class TerrainPageDecorator {
public:
    virtual ~TerrainPageDecorator() = default;
    /// @param pageKey 页的 z/x/y(schemeId 不参与页 key 打包,故为缺省值)
    /// @param target  页存储的共享 array 纹理
    /// @param layer   本页占用的层
    /// @param outDidGpuWork 非空时置为「本次是否真的画了(消耗了 GPU 预算)」。
    ///        **返回 false 有三种成本截然不同的情形**:真画了、在途等待(零成本)、
    ///        资源满被拒(零成本)。页存储的每帧预算只该记前者 —— 否则零成本的
    ///        重试会吃光预算,已就绪的页永远轮不到被画(饥饿死锁,真机实测
    ///        defer 次数恰好等于每帧预算)。
    /// @return true = 已画(或确认本页无内容可画);false = 未就绪,下帧再叫
    virtual bool decoratePage(const TileKey& pageKey, Texture* target,
                              int layer, bool* outDidGpuWork = nullptr) = 0;
    /// 每帧一次(渲染线程,drainInbox 之前)。实现方在此把 worker 产出的 CPU
    /// 数据传上 GPU —— 页存储保证它先于本帧的任何 decoratePage 调用。
    virtual void tickDecorator() {}
    /// 页被 LRU 换租/淘汰时**同步**调用(在页账本移除之后,同一调用路径内)。
    /// 实现方据此放掉该页占用的资源 —— 没有这条通知,只被换租页引用过的源数据
    /// 会一直等一个永远不会再来的 decoratePage,占死实现方的缓存槽位(真机实测
    /// 宽视野 pan 下矢量空窗 ~30s,只能靠帧龄超时兜底回收)。
    virtual void releasePage(const TileKey& pageKey) { (void)pageKey; }
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
class TerrainPageStore {
public:
    struct Config {
        int pageSizeTexels = 256;    // 每层边长(标准 XYZ 影像瓦片 256)
        // B2b 稀疏页存储:array 层数 = LRU 页容量(每页一层,blockLayers=1)。
        // 512×256²×4 ≈ 128MB VRAM 上限;实际按 LRU 只驻留屏幕可见 ~125-185 页。
        int maxPages = 512;
        int maxUploadsPerFrame = 8;  // 每帧上传层数上限(涓流,勿拖动期冻结)
    };

    TerrainPageStore() = default;
    ~TerrainPageStore();

    TerrainPageStore(const TerrainPageStore&) = delete;
    TerrainPageStore& operator=(const TerrainPageStore&) = delete;

    /// 创建共享 array 纹理(maxPages 层,每页一层)。失败返回 false(调用方短路)。
    bool initialize(RenderDevice* device, const Config& config);

    bool isReady() const { return arrayTexture_ != nullptr; }

    /// C-2c:设置页上传后的 GPU 叠画钩子(nullptr = 不叠画,逐字节走现状)。
    /// 生命周期归调用方,须比页存储活得久。
    void setDecorator(TerrainPageDecorator* decorator) { decorator_ = decorator; }

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
    /// **A 通道 = resident 标志(B2b)**:片元用它作 alphaOver factor —— resident=1 页存储
    /// 覆盖、miss=0 保留 mappedRaster(部分就绪/视锥外 cell 优雅降级,决策② 共存不出洞)。
    /// out 需 ≥4 字节;depth clamp 到 [0, kMaxDetDepthLevels]。
    static void encodeLayerRGBA8(int layer, bool resident, int depth,
                                 uint8_t out[4]);
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
    struct PageEntry {
        int layer = -1;
        PageSourceAssembler assembler;
        std::vector<CancellationToken> fetchTokens;  // 每源一个
        // C-2c:本页的 GPU 叠画是否已完成。每次上传置 false(上传覆盖了叠画结果),
        // decoratePage 成功后置 true;tick 每帧重试未完成的页。
        bool decorated = false;
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
        bool fullyResident = false;  // 全 cell 高清页驻留 = 合批资格(丢 mappedRaster)
        uint64_t lastFrame = 0;  // determination 里 touch;sweep 清非本帧可见瓦片
    };

    static uint64_t packKey(const TileKey& key);

    /// 对已上传但未叠画的页重试 decoratePage(每帧有上限,勿抢 draw 预算)。
    void retryPendingDecorations();
    /// kick 单页的**全部源** fetch(worker 回调把解码影像投进 inbox,带
    /// pageKey+layer+源号)。每源一个 token,存进 entry.fetchTokens 供淘汰时 cancel。
    void kickPageFetches(const TileKey& pageTileKey, uint64_t pageKey, int layer,
                         PageEntry& entry);
    void drainInbox();
    void erasePageEntry(uint64_t pageKey);

    RenderDevice* device_ = nullptr;
    Config config_{};
    std::unique_ptr<Texture> arrayTexture_;

    TerrainPageLayerPool pool_;
    std::unordered_map<uint64_t, PageEntry> pages_;      // pageKey → 页账本
    std::unordered_map<uint64_t, TileIndir> tileIndirs_;  // tileKey → 稀疏间接纹理
    std::unique_ptr<Texture> indirArrayTexture_;  // 合批 Step 2:间接纹理共享 array
    TerrainPageLayerPool indirPool_;              // 间接纹理层 LRU(blockLayers=1)
    uint64_t frameId_ = 0;
    // 本帧 tickDecorator 已跑过的帧号。decoratePage 是真 draw,必须画在叠画方
    // 本帧刚传上 GPU 的网格上;两者次序反了会画到上一帧的(或空的)网格,且没有
    // 任何报错。契约 contracts::Id::PageDecorateOrdering 就查这个。
    uint64_t decoratorTickedFrame_ = std::numeric_limits<uint64_t>::max();
    int uploadedLayerTotal_ = 0;

    // C-1:有序源列表(providers_[0] = 底图,定分块/zoom/最大级)。每帧由
    // determination 刷新;变化时作废全部已合成页(旧页少一层或多一层都是错的)。
    std::vector<RasterOverlayTileProvider*> providers_;
    std::vector<RasterOverlayTileProvider*> detProvidersScratch_;  // 剔 null 复用
    std::shared_ptr<PendingInbox> inbox_;            // 跨线程投递箱(存活于回调)
    TerrainPageDecorator* decorator_ = nullptr;      // C-2c:页上传后叠画(不持有)

    // 门② determination:子瓦片相对 capped 瓦片的最大细分深度上限
    // (屏幕驱动一般 ≤5;cap 防远景/病态 zoom 枚举爆量,gridN ≤ 1<<cap)。
    static constexpr int kMaxDetDepthLevels = 6;
    std::unordered_set<uint64_t> visiblePagesScratch_;  // 每次 determination 复用(dedup/计数)
    std::vector<uint8_t> indirTexelsScratch_;           // 间接纹理上传复用缓冲
    // 合批资格判因:全 cell 驻留是唯一卡点,只有布尔结果时"差一点"与"根本达不到"
    // 分不开。每次 determination 重置,随 PageDet 一起输出。
    int residencyCheckedTiles_ = 0;
    int fullyResidentTiles_ = 0;
    float worstResidentRatio_ = 1.0f;
    uint64_t pageDetFrameCounter_ = 0;                  // 节流 log 用(独立于 frameId_)
    int lastVisiblePageCount_ = 0;

    // ===== determination 缓存(高倾斜 fps:视图+可见瓦片集逐值精确不变 → 跳过逐 cell
    // 几何枚举 OBB/视锥/SSE,只重跑便宜的驻留编码。缓存的是**纯几何结果**(哪些 cell
    // 过筛 + cell→pageKey),它只依赖视图+瓦片(key/zoom/minH/maxH)+threshold,不依赖
    // 页驻留态。**失效判定逐字段 ==**(hash 有碰撞→误判为未变=雷,故用精确比对);
    // 顺序/内容任一变化 → 全 re-walk(安全侧,只会多跑不会用错)。驻留层(pool.acquire/
    // touch/淘汰/kick/编码)**每帧必跑**,故冻结相机下异步到页仍逐帧点亮,无 stale。=====
    struct DetTileSig {  // per-tile 几何签名(决定 kept cells 的全部输入)
        uint64_t tileKey = 0;
        int zoom = 0;
        double minH = 0.0;
        double maxH = 0.0;
        bool operator==(const DetTileSig& o) const {
            return tileKey == o.tileKey && zoom == o.zoom && minH == o.minH &&
                   maxH == o.maxH;
        }
    };
    struct DetKeptCell {  // 缓存的「过视锥」cell(纯几何,不含驻留态)
        int dx = 0;
        int dy = 0;
        int d = 0;         // per-cell 渐变 LOD 深度(§16.3):Z-Za,pageKey/fetchKey=粗祖先
        uint64_t pageKey = 0;
        TileKey fetchKey;  // 影像 provider schemeId 的粗祖先页 key(kick fetch 用)
    };
    struct DetTileCacheEntry {
        int gridN = 1;
        uint64_t lastFrame = 0;  // 访问帧;sweep 清非本帧可见瓦片(同 tileIndirs_)
        std::vector<DetKeptCell> kept;
        // 本瓦片被 SSE 地板剔掉的 cell 数(合批资格用,见 fullyResident)。
        //
        // 必须与 kept 同寿命:几何 walk 只在 det 缓存 miss 时跑,算在缓存外就会在
        // hit 帧丢失,让资格闸看到假的 0。
        //
        // 语义关键:被地板剔掉的 cell **通过了视锥测试** —— 它在屏幕上、会产生
        // 片元,只是被判定"屏幕贡献太小,不值得给页"。视锥外的 cell 不产生片元,
        // 两者对合批的安全性含义完全相反,不能混在一个计数里。
        int sseFloorCulled = 0;
    };
    struct DetTileParam {  // 本帧 per-tile 参数(签名阶段算,walk/encode 复用)
        TilesetTile* tile = nullptr;
        uint64_t tileKeyPacked = 0;
        int zoom = 0;
        int gridN = 0;
        double minH = 0.0;
        double maxH = 0.0;
        SchemeId imgSchemeId;  // 影像 provider 的 schemeId(interned handle)
    };
    std::unordered_map<uint64_t, DetTileCacheEntry> detTileCache_;  // tileKey→几何缓存
    std::vector<DetTileParam> detParamsScratch_;
    std::vector<DetTileSig> detTilesScratch_;
    // 上一帧几何输入签名(逐值精确比对)。几何只依赖:frustum(intersectsOBB)+
    // position(cellDist)+ projection/viewportHeight(SSE)+ threshold + provider + 瓦片集。
    // 存 6 frustum 平面(每面 normal.xyz + distance = 4 double,共 24)= 全朝向含 roll。
    bool detSigValid_ = false;
    double detPos_[3] = {};
    double detPlanes_[24] = {};
    double detProj_[16] = {};
    int detVpH_ = 0;
    double detThreshold_ = 0.0;
    const void* detProvider_ = nullptr;
    std::vector<DetTileSig> detTilesPrev_;
    int lastCulledBySse_ = 0;  // TEMP 诊断:hit 帧复用(walk 跳过故不重算)
};

}  // namespace earth_engine
