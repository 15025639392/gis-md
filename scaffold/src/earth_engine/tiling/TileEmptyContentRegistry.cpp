#include "TileEmptyContentRegistry.h"

namespace earth_engine {

bool TileEmptyContentRegistry::contains(const std::string& cacheKey) const {
    if (cacheKey.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return cacheKeys_.count(cacheKey) > 0;
}

void TileEmptyContentRegistry::insert(const std::string& cacheKey) {
    if (cacheKey.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    cacheKeys_.insert(cacheKey);
}

void TileEmptyContentRegistry::erase(const std::string& cacheKey) {
    std::lock_guard<std::mutex> lock(mutex_);
    cacheKeys_.erase(cacheKey);
}

void TileEmptyContentRegistry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cacheKeys_.clear();
}

size_t TileEmptyContentRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cacheKeys_.size();
}

} // namespace earth_engine
