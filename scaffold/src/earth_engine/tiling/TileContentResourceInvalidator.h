#pragma once

#include <cstdint>

namespace earth_engine {

class TileContentCacheManager;

class TileContentResourceInvalidator {
public:
    TileContentResourceInvalidator(
        uint64_t& resourceRevision,
        TileContentCacheManager& contentCache);

    void markResourcesDirty();

private:
    uint64_t& resourceRevision_;
    TileContentCacheManager& contentCache_;
};

} // namespace earth_engine
