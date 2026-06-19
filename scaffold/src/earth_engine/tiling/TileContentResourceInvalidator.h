#pragma once

#include <cstdint>

namespace earth_engine {

class TileContentCacheManager;
struct TileSelectionReuseState;

class TileContentResourceInvalidator {
public:
    TileContentResourceInvalidator(
        uint64_t& resourceRevision,
        TileContentCacheManager& contentCache,
        TileSelectionReuseState& selectionReuseState);

    void markResourcesDirty();

private:
    uint64_t& resourceRevision_;
    TileContentCacheManager& contentCache_;
    TileSelectionReuseState& selectionReuseState_;
};

} // namespace earth_engine
