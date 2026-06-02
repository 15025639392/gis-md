#include "TileTextureCache.h"

#include <algorithm>

namespace earth_engine {

TileTextureCache::TileTextureCache(RenderDevice* device, size_t maxBytes)
    : device_(device), maxBytes_(maxBytes) {}

TileTextureCache::~TileTextureCache() {
    clear();
}

Texture* TileTextureCache::get(const TileKey& key) {
    std::string cacheKey = makeCacheKey(key);
    auto it = map_.find(cacheKey);
    if (it == map_.end()) return nullptr;

    // 移到 LRU 列表头部（最近使用）
    lruList_.splice(lruList_.begin(), lruList_, it->second);
    return it->second->texture.get();
}

void TileTextureCache::put(const TileKey& key, std::unique_ptr<Texture> texture) {
    if (!texture) return;

    std::string cacheKey = makeCacheKey(key);
    size_t bytes = static_cast<size_t>(texture->width() * texture->height() * 4);  // RGBA

    // 如果已存在，先移除旧的
    auto it = map_.find(cacheKey);
    if (it != map_.end()) {
        totalBytes_ -= it->second->bytes;
        lruList_.erase(it->second);
        map_.erase(it);
    }

    // 淘汰直到有足够空间
    while (totalBytes_ + bytes > maxBytes_ && !lruList_.empty()) {
        // 淘汰 LRU（列表尾部）
        auto& last = lruList_.back();
        totalBytes_ -= last.bytes;
        map_.erase(last.cacheKey);
        lruList_.pop_back();
    }

    // 插入到 LRU 头部
    Entry entry;
    entry.cacheKey = cacheKey;
    entry.texture = std::move(texture);
    entry.bytes = bytes;

    lruList_.push_front(std::move(entry));
    map_[cacheKey] = lruList_.begin();
    totalBytes_ += bytes;
}

bool TileTextureCache::contains(const TileKey& key) const {
    return map_.find(makeCacheKey(key)) != map_.end();
}

void TileTextureCache::evict(size_t targetBytes) {
    while (totalBytes_ > targetBytes && !lruList_.empty()) {
        auto& last = lruList_.back();
        totalBytes_ -= last.bytes;
        map_.erase(last.cacheKey);
        lruList_.pop_back();
    }
}

void TileTextureCache::clear() {
    lruList_.clear();
    map_.clear();
    totalBytes_ = 0;
}

std::string TileTextureCache::makeCacheKey(const TileKey& key) {
    return key.schemeId + "/" +
           std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" +
           std::to_string(key.y);
}

} // namespace earth_engine
