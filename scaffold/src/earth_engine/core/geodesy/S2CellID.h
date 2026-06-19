#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace earth_engine {

class S2CellID {
public:
    static S2CellID fromToken(std::string_view token);
    static S2CellID fromFaceLevelPosition(uint8_t face,
                                          uint32_t level,
                                          uint64_t position);
    static S2CellID fromQuadtreeTileID(uint8_t face,
                                       uint32_t level,
                                       uint32_t x,
                                       uint32_t y);

    explicit S2CellID(uint64_t id) noexcept : id_(id) {}

    bool isValid() const noexcept;
    uint64_t getID() const noexcept { return id_; }
    std::string toToken() const;
    int32_t getLevel() const noexcept;
    uint8_t getFace() const noexcept;

private:
    uint64_t id_ = 0;
};

} // namespace earth_engine
