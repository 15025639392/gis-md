#pragma once

#include "../tiling/TileKey.h"
#include "../platform/bridge/PlatformBridge.h"
#include "../threading/CancellationToken.h"

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace earth_engine {

/// 瓦片 Provider 抽象接口。
/// 负责 URL 构建、网络请求和图片解码。不持有 GPU 资源。
class ImageryProvider {
public:
    virtual ~ImageryProvider() = default;

    /// 唯一标识
    virtual std::string id() const = 0;

    /// Provider 类型
    virtual std::string type() const { return "imagery"; }

    /// 瓦片体系 ID（与 TileScheme::id() 匹配）
    virtual std::string schemeId() const = 0;

    /// 最小/最大 zoom
    virtual int minZoom() const = 0;
    virtual int maxZoom() const = 0;

    /// 瓦片尺寸（像素）
    virtual int tileWidth() const = 0;
    virtual int tileHeight() const = 0;

    /// 构建瓦片请求 URL
    virtual std::string buildUrl(const TileKey& key) const = 0;

    /// Provider 是否可为该逻辑 tile 提供 imagery。
    /// 默认要求 schemeId 匹配且 zoom 在 provider 范围内。
    virtual bool supportsTile(const TileKey& key) const {
        return key.schemeId == schemeId() &&
               key.z >= minZoom() &&
               key.z <= maxZoom();
    }

    /// 将引擎逻辑 TileKey 映射为 provider URL 模板使用的 TileKey。
    /// 标准 XYZ/TMS 通常是 identity；OpenGlobus-Earth 三分区 provider
    /// 必须在这里显式处理 grouped-y 语义。
    virtual TileKey providerKeyForTile(const TileKey& key) const { return key; }

    /// 获取 attribution
    virtual std::string attribution() const { return ""; }

    /// 异步请求瓦片图片。
    /// @param key 瓦片键
    /// @param token 取消令牌
    /// @param callback 完成回调（在后台线程调用）。
    ///        参数：(TileKey, DecodedImage | nullptr)
    using TileCallback = std::function<void(const TileKey&,
                                            std::unique_ptr<DecodedImage>)>;

    virtual void requestTile(const TileKey& key,
                             CancellationToken token,
                             TileCallback callback,
                             HttpRequestPriority priority =
                                 HttpRequestPriority::Normal) = 0;

    /// 同步解码瓦片数据（用于测试/调试 Provider）
    virtual std::unique_ptr<DecodedImage> decodeTile(
        const uint8_t* data, size_t len) = 0;
};

} // namespace earth_engine
