#pragma once

#include <cstdint>
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
    void updateVisiblePages(const SelectorView& view,
                            const std::vector<TilesetTile*>& visibleTiles,
                            RasterOverlayTileProvider* provider,
                            double terrainMaxScreenSpaceError);

    // 诊断:上一次 determination 的唯一可见页数(单测/日志)。
    int lastVisiblePageCount() const { return lastVisiblePageCount_; }

    /// 在 applyPerFrameCommandState 里对每个 terrain 命令调用(**无相机,只 bind**):
    /// 若该瓦片本帧 determination 建了间接纹理(TileIndir)→ 绑 array slot20 + 间接纹理
    /// slot21 + 写 pageStoreParams(enabled=1,gridN);否则不动(mappedRaster fallback)。
    /// determination 已按 cell 粒度编好 resident/miss,此处仅 bind。
    /// overlays 用于首次捕获影像 provider。
    void applyToTerrainCommand(
        RenderCommand& cmd, const TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& overlays);

    Texture* arrayTexture() const { return arrayTexture_.get(); }

    /// 稀疏虚拟纹理间接纹理的 RGBA8 层编码:R=layer&0xFF、G=(layer>>8)&0xFF、B=0、
    /// A=resident?255:0。引擎不支持整数纹理,故用 RGBA8 承载 16 位 layer 索引
    /// (容 ≤ 65535 页)。**A 通道 = resident 标志(B2b)**:片元用它作 alphaOver
    /// factor —— resident=1 页存储覆盖、miss=0 保留 mappedRaster(部分就绪/视锥外
    /// cell 优雅降级,决策② 共存不出洞)。out 需 ≥4 字节。
    static void encodeLayerRGBA8(int layer, bool resident, uint8_t out[4]);
    /// 与片元 shader 解码逐位一致:floor(r*255+0.5)+floor(g*255+0.5)*256 = R+G*256。
    /// 供 host round-trip 单测证明「编 layer → RGBA8 → 解码回 layer」链路正确。
    static int decodeLayerRGBA8(const uint8_t in[4]);

    // --- 诊断(单测/日志用)---
    int residentPageCount() const { return pool_.residentCount(); }
    int uploadedLayerTotal() const { return uploadedLayerTotal_; }

private:
    struct PendingInbox;  // 定义在 .cpp:worker 回调安全投递解码影像

    /// 页粒度账本(B2b):每个屏幕可见影像页 (z,x,y) 一层 + 异步 fetch 状态。
    /// pool.acquire 得 layer → 建 PageEntry → kick fetch → 到达置 uploaded。
    /// 淘汰/换租时 cancel fetch(在途到达经 drain 校验丢弃)。
    struct PageEntry {
        int layer = -1;
        bool uploaded = false;
        CancellationToken fetchToken;
    };

    /// 每个屏幕可见 capped 瓦片的稀疏间接纹理(gridN×gridN RGBA8)。
    /// **每帧在 determination 里按当前 resident 页重建**:cell 命中 resident+uploaded
    /// 页 → 编 RG=layer、A=255;否则(视锥外/未 fetch/未到)→ A=0(miss)。
    /// gridN 随屏幕驱动 zoom 变(换 gridN 时重建纹理)。tile 不再可见 → 清除。
    struct TileIndir {
        std::unique_ptr<Texture> tex;
        int gridN = 1;
        uint64_t lastFrame = 0;  // determination 里 touch;sweep 清非本帧可见瓦片
    };

    static uint64_t packKey(const TileKey& key);
    /// 建 gridN×gridN、RGBA8、NEAREST+Clamp 的间接纹理(初值 = texels)。
    /// 片元经它单次 NEAREST fetch 定位 array 层 + 读 A 通道作 miss 回退 factor。
    std::unique_ptr<Texture> createIndirTexture(int gridN, const uint8_t* texels);
    /// kick 单页影像 fetch(worker 回调把解码影像投进 inbox,带 pageKey+layer)。
    void kickPageFetch(const TileKey& pageTileKey, uint64_t pageKey, int layer,
                       CancellationToken& token);
    void drainInbox();
    void erasePageEntry(uint64_t pageKey);

    RenderDevice* device_ = nullptr;
    Config config_{};
    std::unique_ptr<Texture> arrayTexture_;

    TerrainPageLayerPool pool_;
    std::unordered_map<uint64_t, PageEntry> pages_;      // pageKey → 页账本
    std::unordered_map<uint64_t, TileIndir> tileIndirs_;  // tileKey → 稀疏间接纹理
    uint64_t frameId_ = 0;
    int uploadedLayerTotal_ = 0;

    RasterOverlayTileProvider* provider_ = nullptr;  // determination/render 首次捕获
    std::shared_ptr<PendingInbox> inbox_;            // 跨线程投递箱(存活于回调)

    // 门② determination:子瓦片相对 capped 瓦片的最大细分深度上限
    // (屏幕驱动一般 ≤5;cap 防远景/病态 zoom 枚举爆量,gridN ≤ 1<<cap)。
    static constexpr int kMaxDetDepthLevels = 6;
    std::unordered_set<uint64_t> visiblePagesScratch_;  // 每次 determination 复用(dedup/计数)
    std::vector<uint8_t> indirTexelsScratch_;           // 间接纹理上传复用缓冲
    uint64_t pageDetFrameCounter_ = 0;                  // 节流 log 用(独立于 frameId_)
    int lastVisiblePageCount_ = 0;
};

}  // namespace earth_engine
