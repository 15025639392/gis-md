#pragma once

#include "ImageryProvider.h"

namespace earth_engine {

/// 调试用瓦片 Provider。
/// 不依赖网络，为每个 z/x/y 生成彩色棋盘格瓦片。
/// 用于验证瓦片渲染管线（选择、缓存、贴图）。
class DebugImageryProvider : public ImageryProvider {
public:
    DebugImageryProvider();

    std::string id() const override { return "debug-imagery"; }
    std::string type() const override { return "debug-imagery"; }
    std::string schemeId() const override { return "XYZ-WebMercator"; }

    int minZoom() const override { return 0; }
    int maxZoom() const override { return 18; }
    int tileWidth() const override { return 256; }
    int tileHeight() const override { return 256; }

    std::string buildUrl(const TileKey& key) const override;

    void requestTile(const TileKey& key,
                     CancellationToken token,
                     TileCallback callback,
                     HttpRequestPriority priority =
                         HttpRequestPriority::Normal) override;

    std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t* data, size_t len) override;

private:
    /// 同步生成瓦片像素
    static std::unique_ptr<DecodedImage> generateTile(const TileKey& key,
                                                       int width, int height);
};

} // namespace earth_engine
