#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_set>

namespace earth_engine {

class TileEmptyContentRegistry {
public:
    bool contains(const std::string& cacheKey) const;
    void insert(const std::string& cacheKey);
    void erase(const std::string& cacheKey);
    void clear();
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::unordered_set<std::string> cacheKeys_;
};

} // namespace earth_engine
