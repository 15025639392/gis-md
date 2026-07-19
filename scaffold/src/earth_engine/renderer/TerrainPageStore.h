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

/// 北极星合成方案「页存储」生产原型 → 多瓦片页表(门③ Step 3 + §14.1 item 1)。
///
/// 拥有一张共享 `texture2DArray`(§13,每层一页,天然消灭页缝),按 LRU 把层块分给
/// **每个可见 capped 真实地形瓦片**:该瓦片异步拉它在更深 LOD(z + depthLevels)的
/// gridN×gridN 覆盖影像子瓦片 → 灌进自己那块连续层 → 片元按 per-tile pageStoreParams
/// (enabled/gridN/layerBase)单次 array 采样 → capped 粗瓦片显示比其几何 LOD 更细的
/// 真实影像(①路径通)。
///
/// **决策② 已拍板 = 共存/分层 override**:mappedRaster 对所有瓦片继续算(祖先影像
/// fallback);页存储仅在该瓦片**已有 resident 层**时置 enabled=1 接管。page-in 延迟期
/// 显祖先(糊但有)不出洞 = 优雅降级(§12.5#4)。非页存储瓦片逐字节走现状,零回归。
///
/// **决策 单瓦片 → 多瓦片(本文件重写)**:原单 targetKey 锁定改为 per-tile 页表
/// (TerrainPageLayerPool + entries_)。gridN 现固定(config.depthLevels);Step B 改
/// 屏幕驱动 per-tile 深度(复用 chooseQuadtreeSourceZoom)后 gridN 可变、升级 buddy 池。
///
/// ⚠️ **固定 depth 在地平线过取**(122 瓦片 × gridN² 页 ≫ 峰值工作集 185),必须 Step B
/// 屏幕驱动深度才内存安全;近景/中景(3-6 瓦片)当前固定 depth 已正确。
class TerrainPageStore {
public:
    struct Config {
        int pageSizeTexels = 256;    // 每层边长(标准 XYZ 影像瓦片 256)
        int depthLevels = 2;         // 影像比瓦片深几级(gridN = 1<<depthLevels)
        int maxResidentTiles = 16;   // 层池块数(= 同时最多几个 capped 瓦片走页存储)
        int maxUploadsPerFrame = 8;  // 每帧上传层数上限(涓流,勿拖动期冻结)
    };

    TerrainPageStore() = default;
    ~TerrainPageStore();

    TerrainPageStore(const TerrainPageStore&) = delete;
    TerrainPageStore& operator=(const TerrainPageStore&) = delete;

    /// 创建共享 array 纹理(blockCount*gridN² 层)。失败返回 false(调用方短路)。
    bool initialize(RenderDevice* device, const Config& config);

    bool isReady() const { return arrayTexture_ != nullptr; }

    /// 每帧(渲染线程,render 之前):推进帧号、kick 未启动的 per-tile 影像 fetch、
    /// 排空已到达影像(限 maxUploadsPerFrame)灌对应 layer、剔除失效 entry。
    void tick();

    // ==================== 门② 屏幕可见影像页 determination(Step B2a)====================
    // 纯读 + 插桩:遍历本帧可见 capped 真实地形瓦片,对每个瓦片选屏幕合适影像 zoom、
    // 枚举其 gridN×gridN mercator 子瓦片、视锥剔除 → 汇成唯一可见页 (z,x,y) 集合并
    // log 页数/zRange。**不碰池/fetch/间接纹理/render**;供 B2b 接页存储前先隔离验证
    // 页集大小(真机应 ≈ 近景 68 / 地平线 185)。

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

    /// 在 applyPerFrameCommandState 里对每个 terrain 命令调用:capped 真实地形瓦片
    /// 认领(或复用)一块层、touch,若已有 resident 层则绑 array + 写 per-tile
    /// pageStoreParams(enabled=1);否则不动(mappedRaster fallback)。
    /// overlays 用于首次捕获影像 provider。
    void applyToTerrainCommand(
        RenderCommand& cmd, const TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& overlays);

    Texture* arrayTexture() const { return arrayTexture_.get(); }

    /// 稀疏虚拟纹理(Step B1)间接纹理的 RGBA8 层编码:R=layer&0xFF、
    /// G=(layer>>8)&0xFF、B=0、A=255。引擎不支持整数纹理,故用 RGBA8 承载
    /// 16 位 layer 索引(容 gridN²×maxResidentTiles ≤ 65535 页)。out 需 ≥4 字节。
    static void encodeLayerRGBA8(int layer, uint8_t out[4]);
    /// 与片元 shader 解码逐位一致:floor(r*255+0.5)+floor(g*255+0.5)*256 = R+G*256。
    /// 供 host round-trip 单测证明「编 layer → RGBA8 → 解码回 layer」链路正确。
    static int decodeLayerRGBA8(const uint8_t in[4]);

    // --- 诊断(单测/日志用)---
    int residentTileCount() const { return pool_.residentCount(); }
    int uploadedLayerTotal() const { return uploadedLayerTotal_; }

private:
    struct PendingInbox;  // 定义在 .cpp:worker 回调安全投递解码影像

    /// 每个走页存储的 capped 瓦片的账本(层块 + 异步 fetch 状态)。
    struct TileEntry {
        TileKey key{};
        int layerBase = -1;
        int gridN = 1;
        int depthLevels = 0;
        Rectangle bounds{0.0, 0.0, 0.0, 0.0};
        int targetZ = 0;
        bool fetchKicked = false;
        int uploadedLayers = 0;
        CancellationToken fetchToken;  // 淘汰/析构时 cancel(在途 fetch 丢弃)
        // Step B1:per-tile 间接纹理(gridN×gridN RGBA8)。dense 填 cell→连续
        // layer,片元经它单次 NEAREST fetch 定位层(替代闭式公式)。layerBase/
        // gridN 在 entry 存活期固定 → 创建时一次填好即可,换租(淘汰)时随 entry 销毁。
        std::unique_ptr<Texture> indirTexture;
    };

    static uint64_t packKey(const TileKey& key);
    /// 建 entry 的间接纹理:gridN×gridN、texel[cy][cx]=encode(layerBase+cy*gridN+cx)、
    /// NEAREST+Clamp。dense 填 → 与闭式公式逐 cell 等价(B1 隔离验证前提)。
    std::unique_ptr<Texture> buildIndirTexture(int layerBase, int gridN);
    void kickImageryFetch(TileEntry& entry);
    void drainInbox();
    void eraseEntry(uint64_t packed);

    RenderDevice* device_ = nullptr;
    Config config_{};
    int gridN_ = 1;
    std::unique_ptr<Texture> arrayTexture_;

    TerrainPageLayerPool pool_;
    std::unordered_map<uint64_t, TileEntry> entries_;  // packed key → entry
    uint64_t frameId_ = 0;
    int uploadedLayerTotal_ = 0;

    RasterOverlayTileProvider* provider_ = nullptr;  // 首帧从 overlays 捕获
    std::shared_ptr<PendingInbox> inbox_;            // 跨线程投递箱(存活于回调)

    // 门② determination(B2a):子瓦片相对 capped 瓦片的最大细分深度上限
    // (屏幕驱动一般 ≤5;cap 防远景/病态 zoom 枚举爆量,gridN ≤ 1<<cap)。
    static constexpr int kMaxDetDepthLevels = 6;
    std::unordered_set<uint64_t> visiblePagesScratch_;  // 每次 determination 复用
    std::vector<TileKey> subtileScratch_;               // 枚举复用缓冲
    uint64_t pageDetFrameCounter_ = 0;                  // 节流 log 用(独立于 frameId_)
    int lastVisiblePageCount_ = 0;
};

}  // namespace earth_engine
