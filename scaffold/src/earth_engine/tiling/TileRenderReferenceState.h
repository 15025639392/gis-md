#pragma once

#include <cstdint>

namespace earth_engine {

class TileRenderReferenceState {
public:
    int32_t count() const { return count_; }
    uint64_t lastUsedFrame() const { return lastUsedFrame_; }
    void add() { ++count_; }
    void remove() {
        --count_;
        if (count_ < 0) {
            count_ = 0;
        }
    }
    void clear() { count_ = 0; }
    void markUsedForFrame(uint64_t frameNumber) {
        lastUsedFrame_ = frameNumber;
    }

private:
    int32_t count_ = 0;
    uint64_t lastUsedFrame_ = 0;
};

} // namespace earth_engine
