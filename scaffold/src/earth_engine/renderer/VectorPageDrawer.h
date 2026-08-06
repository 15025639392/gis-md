#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../data/VectorTileMeshBuilder.h"
#include "../tiling/TileKey.h"
#include "TerrainPageStore.h"

namespace earth_engine {

class Buffer;
class RenderDevice;
class Renderer;
class Framebuffer;
class ThreadPool;

/// 矢量在**页原生分辨率**上直接画进页存储 array 层(C-2c-3,C-2 的收尾)。
///
/// 这条路取代「CPU 栅格化成影像再当一个源合成」:后者把瓦片烘成固定分辨率位图,
/// 页 zoom 深于源 zoom 时只能放大 —— 真机实测 z14 源贴 z17 页放大 8 倍,路网糊成
/// 灰带。这里源瓦片只镶嵌一次(C-2b,瓦片本地归一化坐标),每个页拿同一份网格配
/// 不同的正交矩阵画一遍,分辨率永远是页自己的。
///
/// 线程:fetch 回调(网络线程)→ 解码 + 建网格(worker)→ 投递箱 →
/// `tick()`(渲染线程)建 GPU buffer → `decoratePage()`(渲染线程)画。
/// GPU 资源的创建与使用全在渲染线程,worker 只碰纯 CPU 数据。
class VectorPageDrawer : public TerrainPageDecorator {
public:
    struct Options {
        VectorRasterStyle style;
        /// 数据侧的源瓦片上限。页比它深时取祖先瓦片 + 子矩形正交投影 ——
        /// **不放大位图**,几何还是原来的几何。
        int maxSourceZoom = 14;
        int pageSizeTexels = 256;
        /// 源瓦片网格的 LRU 容量。页 zoom ≤ maxSourceZoom 时页与源 1:1 **不共享**
        /// (宽视野下可见页数可远超此值,真机 z9-10 实测 291 页)——收敛不靠
        /// 容量,靠准入控制:未消费的瓦片受保护不可淘汰,满员时拒绝新准入,
        /// 已准入者保证至少服务一次后才可能被换出。
        size_t maxCachedTiles = 96;
    };

    /// 与 demo 既有的 MVT 拉取同签名(statusCode + body)。
    using FetchFn = std::function<void(
        const TileKey&, std::function<void(int, std::vector<uint8_t>)>)>;

    VectorPageDrawer(RenderDevice* device, Renderer* renderer, ThreadPool* pool,
                     Options options, FetchFn fetch);
    ~VectorPageDrawer() override;

    VectorPageDrawer(const VectorPageDrawer&) = delete;
    VectorPageDrawer& operator=(const VectorPageDrawer&) = delete;

    /// 渲染线程每帧一次:把 worker 建好的网格传上 GPU(每帧有上限,涓流)。
    /// 由页存储在本帧任何 decoratePage 之前调用。
    void tickDecorator() override;

    // TerrainPageDecorator
    bool decoratePage(const TileKey& pageKey, Texture* target,
                      int layer) override;

    // --- 诊断 ---
    int readyTileCount() const { return readyTiles_; }
    int drawnPageCount() const { return drawnPages_; }

    /// 页在源瓦片内的子矩形 → 正交矩阵(列主序 16 float)。
    /// `flipY` = 目标行 0 在 NDC 的哪一侧:GL 的 FBO 原点在左下(false),
    /// Metal 的 render target 原点在左上(true)。**后端差异只在这里**,
    /// 两份 shader 逐字符相同。纯函数,可 host 单测。
    static void makePageOrtho(double originX, double originY, double span,
                              bool flipY, float outMatrix[16]);

private:
    enum class TileState { Pending, Ready, Empty, Failed };

    struct GpuTile {
        TileState state = TileState::Pending;
        std::unique_ptr<Buffer> vertexBuffer;
        std::unique_ptr<Buffer> indexBuffer;
        int indexCount = 0;
        /// 至少服务过一个页(画过 / Empty 或 Failed 被查询过)。未消费的瓦片
        /// 受淘汰保护 —— 这是宽视野收敛的不变量:没有它,可见页数超过容量时
        /// fetch→evict→重拉 永不停(真机 z9-10 实测 drawn 恒 0、evict 风暴)。
        bool consumed = false;
        /// 准入时的 tick。页被页存储换租后其瓦片永远等不到消费,保护到期
        /// (kProtectionTicks)后回归普通 LRU 语义,避免僵尸条目占死槽位。
        uint64_t admittedTick = 0;
    };

    struct Inbox;

    void kickFetch(const TileKey& srcKey, uint64_t srcPacked);
    /// LRU touch(移到最近端)。不淘汰 —— 淘汰只发生在准入时的 tryEvictOne。
    void touch(uint64_t srcPacked);
    /// 从最久未用端找第一个可淘汰(已消费或保护到期)的瓦片换出。
    /// 找不到 = 全员受保护,调用方必须拒绝准入而不是硬挤。
    bool tryEvictOne();
    bool ensureFramebuffer(Texture* target);

    RenderDevice* device_ = nullptr;
    Renderer* renderer_ = nullptr;
    ThreadPool* pool_ = nullptr;
    Options options_;
    FetchFn fetch_;

    std::unordered_map<uint64_t, GpuTile> tiles_;
    std::list<uint64_t> lru_;  // 前=最近用
    std::unordered_map<uint64_t, std::list<uint64_t>::iterator> lruPos_;
    std::shared_ptr<Inbox> inbox_;

    std::unique_ptr<Framebuffer> framebuffer_;
    Texture* framebufferTarget_ = nullptr;  // 建 FBO 时绑的 array 纹理

    int readyTiles_ = 0;
    int drawnPages_ = 0;
    // 判因计数器:区分「LRU thrash(反复重拉同一批源瓦片)」与「291 个 pass
    // 本身就贵」—— 两者修法完全不同,靠猜会修错。
    int fetchesKicked_ = 0;
    int evictions_ = 0;
    /// 满员且无可淘汰者时被拒绝的准入次数(页存储下轮再来)。持续 > 0 且
    /// drawn 也在涨 = 正常排队消化;drawn 恒 0 才是异常。
    int admissionDeferrals_ = 0;
    uint64_t tickCounter_ = 0;
    int lastDrawn_ = 0;
    int lastFetches_ = 0;
    int lastDeferrals_ = 0;
};

}  // namespace earth_engine
