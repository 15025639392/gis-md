#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ImageryProvider.h"
#include "../data/VectorTileRasterizer.h"

namespace earth_engine {

class ThreadPool;

/// 矢量瓦片当影像用的 Provider(E4 贴地双轨制的第二段)。
///
/// **这是整个 E4 的支点**:ImageryProvider 的契约是「给个 key,回个
/// DecodedImage」—— 没有任何东西要求它走 HTTP 或返回真实照片。于是矢量瓦片
/// 栅格化后可以直接冒充影像,**整套 raster overlay → 地形合成管线**
/// (TerrainPageStore / 上采样 / LOD / mapping / 缓存)原样复用,贴地免费。
///
/// 对比另外两条贴地路线:
/// - 逐顶点采地形高(P3 方案 A):要地形采样空间索引,LOD 切换还得重钳;
/// - stencil 分类(P6 方案 B):像素级精确,但每要素一个挤出体,底图万级量
///   撑不住,且 Metal 侧未接线。
/// 本路线的代价是**失去分辨率无关性**(放大会糊),故它服务底图大宗要素;
/// 编辑层与少量要素继续走几何路径,这就是「双轨制」。
///
/// 线程契约:requestTile 可在任意线程调用;拉取回调、解码、栅格化都在后台
/// (线程池未注入时就地在拉取回调线程做,仍不占渲染线程);TileCallback 按
/// ImageryProvider 约定在后台线程触发。样式在构造时快照,provider 存活期内
/// 不变 —— 要换样式就换 provider,免得给纯函数栅格器加锁。
class VectorImageryProvider : public ImageryProvider {
public:
    /// (statusCode, body):200 且非空视为成功。与 MvtVectorSource 同签名,
    /// demo 可以共用同一个 fetch 实现。
    using FetchCallback = std::function<void(int, std::vector<uint8_t>)>;
    using FetchFn = std::function<void(const TileKey&, FetchCallback)>;

    struct Options {
        std::string id = "vector-imagery";
        /// 必须与地形/影像的瓦片体系一致,否则 overlay 映射对不上。
        std::string schemeId = "XYZ-WebMercator";
        int minZoom = 0;
        int maxZoom = 14;
        /// 输出纹理边长。512 是清晰度与内存的常用折中;矢量烘图不像照片,
        /// 256 下细路会糊成一片。
        int tileSize = 512;
        VectorRasterStyle style;
    };

    VectorImageryProvider(Options options, FetchFn fetch,
                          ThreadPool* rasterPool = nullptr);

    std::string id() const override { return options_.id; }
    std::string type() const override { return "vector-imagery"; }
    std::string schemeId() const override { return options_.schemeId; }
    int minZoom() const override { return options_.minZoom; }
    int maxZoom() const override { return options_.maxZoom; }
    int tileWidth() const override { return options_.tileSize; }
    int tileHeight() const override { return options_.tileSize; }

    /// 没有真实 URL 概念(拉取由注入的 FetchFn 负责)。返回一个可读标识,
    /// 只用于诊断/日志 —— 引擎不拿它发请求。
    std::string buildUrl(const TileKey& key) const override;

    void requestTile(const TileKey& key,
                     CancellationToken token,
                     TileCallback callback,
                     HttpRequestPriority priority =
                         HttpRequestPriority::Normal) override;

    /// 同步:MVT 字节 → 栅格图。供测试与调试(也是 ImageryProvider 契约)。
    /// ⚠️ 无 zoom 上下文,按 maxZoom 求值样式的 zoom 相关部分 —— 故它只适合
    /// 调试,生产路径走 requestTile(带真实瓦片 z)。
    std::unique_ptr<DecodedImage> decodeTile(const uint8_t* data,
                                             size_t len) override;

    /// 指定 zoom 的同步栅格化(测试用,避开上面那条 zoom 近似)。
    std::unique_ptr<DecodedImage> decodeTileAtZoom(const uint8_t* data,
                                                   size_t len, int zoom) const;

private:
    Options options_;
    FetchFn fetch_;
    ThreadPool* rasterPool_ = nullptr;
};

} // namespace earth_engine
