#pragma once

#include "TerrainProvider.h"
#include <atomic>
#include <string>

namespace earth_engine {

class PlatformBridge;

/// 高度图地形 Provider。
///
/// 支持 Mapzen Terrarium 和 Mapbox Terrain-RGB 两种 RGB 高程编码。
///
/// Terrarium:
///   height(m) = R*256 + G + B/256 - 32768
///
/// Mapbox Terrain-RGB:
///   height(m) = -10000 + (R*256*256 + G*256 + B) * 0.1
///
/// URL 模板支持 {z}/{x}/{y} 占位符。
/// 示例：
///   https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png
///   file:///data/local/tmp/tiles/fabdem-raster-dem/{z}/{x}/{y}.png
class HeightmapTerrainProvider : public TerrainProvider {
public:
    enum class Encoding {
        Terrarium,
        MapboxTerrainRgb
    };

    /// @param urlTemplate URL 模板
    /// @param attribution 版权声明
    explicit HeightmapTerrainProvider(std::string urlTemplate,
                                       std::string attribution = "");
    ~HeightmapTerrainProvider() override;

    /// 注入平台 HTTP 桥接
    void setPlatformBridge(PlatformBridge* bridge);

    std::string id() const override;
    std::string type() const override { return "heightmap-terrain"; }
    std::string schemeId() const override { return "XYZ-WebMercator"; }

    int minZoom() const override { return minZoom_; }
    int maxZoom() const override { return maxZoom_; }
    int tileSize() const override { return tileSize_; }
    std::string attribution() const override { return attribution_; }

    void setZoomRange(int minZ, int maxZ);
    void setMaxNativeZoom(int maxNativeZ) { maxNativeZoom_ = maxNativeZ; }
    int maxNativeZoom() const { return maxNativeZoom_; }
    void setEncoding(Encoding encoding);
    void setTileSize(int size) { tileSize_ = size; }
    void setHeightFactor(float factor) { heightFactor_ = factor; }
    // 解码出的 heightmap 的边界内缩(像素),写进 DecodedHeightmap::borderInset。
    // 0 = 顶点栅格源(自产 grid65);0.5 = cell-registered + 1px 重叠环
    // (Mapbox 514 Terrain-RGB)。见 DecodedHeightmap::borderInset 注释。
    void setBorderInset(float insetPixels) { borderInset_ = insetPixels; }
    void setNoDataValues(std::vector<float> values) { noDataValues_ = std::move(values); }

    /// Terrain-RGB 的 nodata 底值(米,未乘 heightFactor):RGB(0,0,0) 的解码结果。
    /// 缺邻居的重叠环与数据空洞都编成它。这是**哨兵值**的单一来源 —— 注册点与
    /// 下游的契约检查(contracts::Id::DemNodataSentinel)共用,勿各自写字面量。
    static constexpr float kTerrainRgbNoDataFloorMeters = -10000.0f;

    /// 该编码是否在解码时**隐式**追加一个 nodata 哨兵。
    ///
    /// Terrain-RGB 的 RGB(0,0,0) 解码恰为 -10000m = 数据源 nodata 底值,解码器
    /// 无条件注册它(见 decodeHeightmap)。所以「实际生效的哨兵表」≠「配置里写
    /// 的那几个值」——环境快照曾因直接报配置列表长度而在哨兵已注册时显示 0。
    /// 判据收在这里,注册点与观测点共用,勿各自写 `== MapboxTerrainRgb`。
    static bool hasImplicitNoDataSentinel(Encoding encoding) {
        return encoding == Encoding::MapboxTerrainRgb;
    }

    /// 解码时实际生效的哨兵个数 = 显式配置值个数 + 隐式哨兵。
    static int effectiveNoDataCount(Encoding encoding, size_t configuredCount) {
        return static_cast<int>(configuredCount) +
               (hasImplicitNoDataSentinel(encoding) ? 1 : 0);
    }

    std::string buildUrl(const TileKey& key) const override;

    void requestTile(const TileKey& key,
                     CancellationToken token,
                     TerrainCallback callback,
                     HttpRequestPriority priority =
                         HttpRequestPriority::Normal) override;

    ProviderRequestDiagnostics requestDiagnostics() const override;

    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t* data, size_t len) override;

private:
    std::vector<uint8_t> httpGet(const std::string& url,
                                 const CancellationToken& token,
                                 HttpRequestPriority priority);

    std::string urlTemplate_;
    std::string attribution_;
    Encoding encoding_ = Encoding::Terrarium;
    int minZoom_ = 0;
    int maxZoom_ = 14;
    int maxNativeZoom_ = 14;
    int tileSize_ = 256;
    float heightFactor_ = 1.0f;
    float borderInset_ = 0.0f;
    std::vector<float> noDataValues_;
    PlatformBridge* platformBridge_ = nullptr;
    std::atomic<int> requestsStarted_{0};
    std::atomic<int> requestsCompleted_{0};
    std::atomic<int> activeWorkerBlockingRequests_{0};
    std::atomic<int> peakWorkerBlockingRequests_{0};
};

} // namespace earth_engine
