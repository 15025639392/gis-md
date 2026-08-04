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
        /// 源瓦片网格的 LRU 容量。一个页只碰一份源瓦片,可见页 ~185 个但共享
        /// 祖先后源瓦片数远少于此。
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
    };

    struct Inbox;

    void kickFetch(const TileKey& srcKey, uint64_t srcPacked);
    /// LRU:超容量时淘汰最久未用的源瓦片(GPU buffer 随之释放)。
    void touchAndTrim(uint64_t srcPacked);
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
};

}  // namespace earth_engine
