#pragma once

#include "RenderDevice.h"
#include "../tiling/TileKey.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <list>
#include <cstddef>

namespace earth_engine {

/// LRU GPU 纹理缓存。
/// 缓存 TileKey → Texture 的映射，限制总内存使用。
/// 非线程安全 — 应在主线程/渲染线程访问。
class TileTextureCache {
public:
    /// @param device 渲染设备（用于创建纹理，不拥有所有权）
    /// @param maxBytes 最大缓存字节数（默认 64 MB）
    explicit TileTextureCache(RenderDevice* device,
                              size_t maxBytes = 64 * 1024 * 1024);
    TileTextureCache(RenderDevice* device,
                     std::string cacheDomain,
                     size_t maxBytes = 64 * 1024 * 1024);
    ~TileTextureCache();

    // 禁止拷贝
    TileTextureCache(const TileTextureCache&) = delete;
    TileTextureCache& operator=(const TileTextureCache&) = delete;

    /// 获取缓存的纹理。
    /// @return 纹理指针，未命中返回 nullptr
    Texture* get(const TileKey& key);

    /// 存入纹理。
    /// 如果超过容量上限，会触发 LRU 淘汰。
    void put(const TileKey& key, std::unique_ptr<Texture> texture);

    /// 检查是否已缓存
    bool contains(const TileKey& key) const;

    /// 淘汰最旧的条目直到低于 maxBytes 或无可淘汰
    void evict(size_t targetBytes);

    /// 清除所有缓存条目
    void clear();

    /// 当前缓存大小（字节）
    size_t totalBytes() const { return totalBytes_; }

    /// 缓存条目数
    size_t count() const { return map_.size(); }

    /// 最大容量（字节）
    size_t maxBytes() const { return maxBytes_; }

private:
    struct Entry {
        std::string cacheKey;
        std::unique_ptr<Texture> texture;
        size_t bytes;
    };

    /// 从 TileKey 生成缓存键
    std::string makeCacheKey(const TileKey& key) const;

    std::string cacheDomain_;
    size_t maxBytes_;
    size_t totalBytes_ = 0;

    using ListType = std::list<Entry>;
    using MapType = std::unordered_map<std::string, ListType::iterator>;

    ListType lruList_;
    MapType map_;
};

} // namespace earth_engine
