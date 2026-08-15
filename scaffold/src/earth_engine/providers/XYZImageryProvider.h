#pragma once

#include "ImageryProvider.h"
#include <atomic>
#include <mutex>
#include <string>

namespace earth_engine {

class PlatformBridge;

/// 标准 XYZ 瓦片 Provider。
/// 使用 HTTP GET 获取 PNG 瓦片，stb_image 解码为 RGBA。
/// URL 模板示例：
///   OSM:     https://tile.openstreetmap.org/{z}/{x}/{y}.png
///   高德卫星: https://webst0{s}.is.autonavi.com/appmaptile?style=6&x={x}&y={y}&z={z}
///
/// 支持占位符：{z} {x} {y} {s}（子域名 0-3）
class XYZImageryProvider : public ImageryProvider {
public:
    /// @param urlTemplate URL 模板
    /// @param attribution 版权声明
    explicit XYZImageryProvider(std::string urlTemplate,
                                std::string attribution = "");
    ~XYZImageryProvider() override;

    /// 注入平台桥接（Android 网络由 native curl scheduler 统一调度）。
    void setPlatformBridge(PlatformBridge* bridge);

    std::string id() const override;
    std::string type() const override { return "xyz-imagery"; }
    std::string schemeId() const override { return schemeId_; }

    int minZoom() const override { return minZoom_; }
    int maxZoom() const override { return maxZoom_; }
    int tileWidth() const override { return tileWidth_; }
    int tileHeight() const override { return tileHeight_; }

    void setZoomRange(int minZoom, int maxZoom);
    void setTileSize(int width, int height);
    void setSchemeId(std::string schemeId);
    void setOpenGlobusGroupedY(bool enabled);
    void setOpenGlobusPolarGroupsEnabled(bool enabled);

    std::string buildUrl(const TileKey& key) const override;
    bool supportsTile(const TileKey& key) const override;
    TileKey providerKeyForTile(const TileKey& key) const override;
    std::string attribution() const override;

    void requestTile(const TileKey& key,
                     CancellationToken token,
                     TileCallback callback,
                     HttpRequestPriority priority =
                         HttpRequestPriority::Normal) override;

    /// HTTP 缓存命中则直接回调(下沉 worker 解码)并返回 true;
    /// 未命中/坏体返回 false,调用方继续走网络。语义见 .cpp 注释。
    bool tryServeFromHttpCache(const TileKey& key,
                               const CancellationToken& token,
                               const TileCallback& callback,
                               const std::string& url);

    ProviderRequestDiagnostics requestDiagnostics() const override;

    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t* data, size_t len) override;

protected:
    void setAttribution(std::string attribution);
    PlatformBridge* platformBridge() const { return platformBridge_; }

private:
    std::string urlTemplate_;
    std::string attribution_;
    mutable std::mutex attributionMutex_;
    int minZoom_ = 0;
    int maxZoom_ = 25;
    int tileWidth_ = 256;
    int tileHeight_ = 256;
    std::string schemeId_ = "XYZ-WebMercator";
    bool openGlobusGroupedY_ = false;
    bool openGlobusPolarGroupsEnabled_ = true;
    PlatformBridge* platformBridge_ = nullptr;
    std::atomic<int> requestsStarted_{0};
    std::atomic<int> requestsCompleted_{0};
    std::atomic<int> activeWorkerBlockingRequests_{0};
    std::atomic<int> peakWorkerBlockingRequests_{0};

};

} // namespace earth_engine
