#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ImageryProvider.h"
#include "../data/MvtTileFetchCache.h"
#include "../data/VectorTileRasterizer.h"

namespace earth_engine {

class ThreadPool;

/// 矢量面 drape 源:MVT 面要素动态栅格化冒充影像,进 TerrainPageStore 页
/// 合成(刀1,2026-08-13 架构定案"面→drape 进页合成")。
///
/// **支点不变**(E4 原话):ImageryProvider 的契约只是「给个 key 回个
/// DecodedImage」。矢量瓦片栅格化后与真影像无区别,整套页存储合成管线
/// (compose worker / LRU / 祖先回退 / 屏幕界定高清页)原样复用,贴地免费。
///
/// **与被删的 E4 版三点本质差异**(死因逐条对应):
/// ① **overzoom 现画**:maxZoom() 谎报到影像级(advertisedMaxZoom),页存储
///    按屏幕清晰度要 z17 就给 z17 —— 内部把请求矩形钳到 dataMaxZoom 取祖先
///    MVT,只画落在矩形里的子区域(rasterizeMvtRect)。E4 版老实报 z14,
///    页存储 scale-bias 放大 → "页纹素封顶"死因就出在这一步。
/// ② **只承载面**:线(路网)对模糊敏感,由 SDF 场+地形 FS 解算承接(刀2);
///    样式里只配 polygon 层。面的边缘质量基线 = 影像级(与卫星底图同轨同糊)。
/// ③ **GCJ-02 源网格适配**:页网格按首源(高德卫星,GCJ-02)建,喂来的 key
///    语义是"GCJ 空间的该矩形"。gcj02SourceGrid=true 时把矩形按中心点
///    toWgs84 平移后再选取 WGS84 的 OSM 矢量瓦 —— 与渲染侧 per-tile 常量
///    worldOffset 修正严格互补,矢量与影像逐像素同位。
///
/// **失败语义(与 E4 版相反,页存储专属)**:fetch/解码失败一律回**全透明图**
/// 而不是 nullptr。页存储的 PageSourceAssembler 收不到某源就永不 complete,
/// 合成缓冲(页边长² ×4B)不释放 —— server 未启动时 512 页 × 256KB 级滞留。
/// 透明图让页正常收口,视觉=纯影像(优雅降级)。代价:该页 LRU 淘汰前不会
/// 重试矢量层(demo 场景可接受;server 恢复后新页自然带面)。
///
/// 线程契约:requestTile 可在任意线程调(渲染线程调用方要求立即返回);
/// 栅格化在 rasterPool(未注入时在拉取回调线程/就地,host 测试用)。
/// MVT 瓦 fetch+decode 走注入的共享 MvtTileFetchCache(刀2 起与路网场烘焙
/// 共用同一批 z14 祖先瓦,不共享就是双份 fetch+decode)。
///
/// **拆除竞态(异步回调只带自持数据,tombstone 法则)**:fetch 回调链只捕
/// shared_ptr(cache 的 State / 本类的 Assembly);线程池持 weak_ptr,锁不上
/// 就地栅格化(拆除路径回调仍必到,页存储账本校验会丢弃迟到结果)。裸指针
/// 版真机崩过:`enqueue on stopped ThreadPool`(demo teardown 先 reset 池,
/// 与 MvtVectorSource 的 tombstone_22 同型)。
class VectorDrapeImageryProvider : public ImageryProvider {
public:
    using FetchCallback = MvtTileFetchCache::FetchCallback;
    using FetchFn = MvtTileFetchCache::FetchFn;

    struct Options {
        std::string id = "vector-drape";
        std::string schemeId = "XYZ-WebMercator";
        int minZoom = 0;
        /// 报给页存储的上限:决定页 determination 允许的最深页 zoom。取影像
        /// 源同级,页要多深矢量就现画多深 —— 这是"动态栅格化"的机关。
        int advertisedMaxZoom = 18;
        /// MVT 数据的真实上限(tippecanoe 切片档)。深于此的请求内部取祖先。
        int dataMaxZoom = 14;
        /// 输出边长。页原生 256:再大页存储 resample 也会缩回 256,白算。
        int tileSize = 256;
        VectorRasterStyle style;
        /// 页网格首源是 GCJ-02(高德)时开:请求矩形先 toWgs84 平移。
        bool gcj02SourceGrid = false;
    };

    VectorDrapeImageryProvider(Options options,
                               std::shared_ptr<MvtTileFetchCache> tileCache,
                               std::shared_ptr<ThreadPool> rasterPool =
                                   nullptr);

    std::string id() const override { return options_.id; }
    std::string type() const override { return "vector-drape"; }
    std::string schemeId() const override { return options_.schemeId; }
    int minZoom() const override { return options_.minZoom; }
    int maxZoom() const override { return options_.advertisedMaxZoom; }
    int tileWidth() const override { return options_.tileSize; }
    int tileHeight() const override { return options_.tileSize; }

    /// 没有真实 URL 概念(拉取由注入的 cache/FetchFn 负责)。仅诊断/日志。
    std::string buildUrl(const TileKey& key) const override;

    void requestTile(const TileKey& key,
                     CancellationToken token,
                     TileCallback callback,
                     HttpRequestPriority priority =
                         HttpRequestPriority::Normal) override;

    /// 同步:MVT 字节 → 栅格图(整瓦,按 dataMaxZoom 求值样式)。测试/调试。
    std::unique_ptr<DecodedImage> decodeTile(const uint8_t* data,
                                             size_t len) override;

    /// V26 一期:运行期换样式(换肤)。线程安全(小锁,requestTile 契约是
    /// 任意线程);在途请求持旧快照收口(Assembly 按值快照),新请求起用
    /// 新样式。**只换生产侧**:已合成进页存储的旧样式页不自动作废——生产者
    /// 不知道谁缓存了它的产物(页存储对 provider 反向零依赖是刻意的),
    /// 调用方随后须 Engine::invalidateComposedTerrainPages() 触发重栅格化。
    void setStyle(VectorRasterStyle style);

private:
    struct Assembly;  // 一次 requestTile 的聚合状态(等多张源瓦到齐)

    // static + 只收 shared 状态:可能在 provider 亡后的迟到回调里执行。
    static void runAssembly(const std::shared_ptr<Assembly>& assembly);
    static void completeIfReady(const std::shared_ptr<Assembly>& assembly);

    /// options_.style 的加锁快照(setStyle 可与任意线程的 requestTile 并发)。
    VectorRasterStyle styleSnapshot() const;

    Options options_;
    mutable std::mutex styleMutex_;  // 只护 options_.style,其余字段构造后只读
    std::shared_ptr<MvtTileFetchCache> tileCache_;
    std::shared_ptr<ThreadPool> rasterPool_;
};

} // namespace earth_engine
