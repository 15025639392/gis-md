#pragma once

#include "ImageryProvider.h"
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

    /// 注入平台 HTTP 桥接（Android 上使用 JNI HTTP 替代 libcurl）
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

    std::string buildUrl(const TileKey& key) const override;
    bool supportsTile(const TileKey& key) const override;
    TileKey providerKeyForTile(const TileKey& key) const override;
    std::string attribution() const override { return attribution_; }

    void requestTile(const TileKey& key,
                     CancellationToken token,
                     TileCallback callback) override;

    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t* data, size_t len) override;

private:
    std::vector<uint8_t> httpGet(const std::string& url,
                                  const std::atomic<bool>* cancelled);

    std::string urlTemplate_;
    std::string attribution_;
    int minZoom_ = 0;
    int maxZoom_ = 19;
    int tileWidth_ = 256;
    int tileHeight_ = 256;
    std::string schemeId_ = "XYZ-WebMercator";
    bool openGlobusGroupedY_ = false;
    PlatformBridge* platformBridge_ = nullptr;
};

} // namespace earth_engine
