#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ImageryProvider.h"
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
/// 内部 MvtTile 解码缓存 + 在途合并:overzoom 下同一张 z14 祖先瓦被多个页
/// 并发请求是常态,不合并会对同一瓦片重复 fetch+decode。
///
/// **拆除竞态(异步回调只带自持数据,tombstone 法则)**:fetch 回调来自
/// 网络线程,可迟到于 GL 会话拆除 —— 回调捕 shared_ptr<State>(缓存/在途
/// 账本)而非 this;线程池持 weak_ptr,锁不上就地栅格化(拆除路径回调仍
/// 必到,页存储账本校验会丢弃迟到结果)。裸指针版真机崩过:
/// `enqueue on stopped ThreadPool`(demo teardown 先 reset 池,与
/// MvtVectorSource 的 tombstone_22 同型)。
class VectorDrapeImageryProvider : public ImageryProvider {
public:
    /// (statusCode, body):200 且非空视为成功。与 MvtVectorSource 同签名,
    /// demo 可以共用同一个 fetch 实现。
    using FetchCallback = std::function<void(int, std::vector<uint8_t>)>;
    using FetchFn = std::function<void(const TileKey&, FetchCallback)>;

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
        /// 解码后 MvtTile 缓存容量(张)。z14 瓦 ~9km 见方,48 张覆盖
        /// 400km² 级别,远超单视野。
        size_t tileCacheCapacity = 48;
    };

    VectorDrapeImageryProvider(Options options, FetchFn fetch,
                               std::shared_ptr<ThreadPool> rasterPool =
                                   nullptr);

    std::string id() const override { return options_.id; }
    std::string type() const override { return "vector-drape"; }
    std::string schemeId() const override { return options_.schemeId; }
    int minZoom() const override { return options_.minZoom; }
    int maxZoom() const override { return options_.advertisedMaxZoom; }
    int tileWidth() const override { return options_.tileSize; }
    int tileHeight() const override { return options_.tileSize; }

    /// 没有真实 URL 概念(拉取由注入的 FetchFn 负责)。仅诊断/日志。
    std::string buildUrl(const TileKey& key) const override;

    void requestTile(const TileKey& key,
                     CancellationToken token,
                     TileCallback callback,
                     HttpRequestPriority priority =
                         HttpRequestPriority::Normal) override;

    /// 同步:MVT 字节 → 栅格图(整瓦,按 dataMaxZoom 求值样式)。测试/调试。
    std::unique_ptr<DecodedImage> decodeTile(const uint8_t* data,
                                             size_t len) override;

    /// 诊断:自建缓存的累计命中/拉取数(线程安全快照)。
    struct CacheStats {
        uint64_t hits = 0;
        uint64_t fetches = 0;
    };
    CacheStats cacheStats() const;

private:
    struct Assembly;  // 一次 requestTile 的聚合状态(等多张源瓦到齐)
    struct State;     // 缓存/在途账本(shared:fetch 回调自持,防拆除悬垂)

    // static + 只收 shared 状态:这些可能在 provider 亡后的迟到回调里执行。
    static void runAssembly(const std::shared_ptr<Assembly>& assembly);
    static void completeIfReady(const std::shared_ptr<Assembly>& assembly);
    static void onSourceTileReady(const std::shared_ptr<State>& state,
                                  uint64_t dataKey,
                                  std::shared_ptr<const MvtTile> tile);

    Options options_;
    FetchFn fetch_;
    std::shared_ptr<ThreadPool> rasterPool_;
    std::shared_ptr<State> state_;
};

} // namespace earth_engine
