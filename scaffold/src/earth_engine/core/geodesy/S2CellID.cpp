#include "S2CellID.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace earth_engine {

namespace {

constexpr uint32_t kMaxLevel = 30;

void rotate(uint32_t n, uint32_t& x, uint32_t& y, bool rx, bool ry) {
    if (ry) {
        return;
    }
    if (rx) {
        x = n - 1 - x;
        y = n - 1 - y;
    }
    std::swap(x, y);
}

uint64_t encodeHilbert2D(uint32_t level, uint32_t x, uint32_t y) {
    const uint32_t n = uint32_t{1} << level;
    uint64_t index = 0;

    for (uint64_t s = n >> 1U; s > 0; s >>= 1U) {
        const bool rx = (x & s) > 0;
        const bool ry = (y & s) > 0;

        index += ((3ULL * static_cast<uint64_t>(rx)) ^
                  static_cast<uint64_t>(ry)) *
                 s * s;
        rotate(n, x, y, rx, ry);
    }

    return index;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

} // namespace

S2CellID S2CellID::fromToken(std::string_view token) {
    if (token.empty() || token.size() > 16) {
        return S2CellID(0);
    }

    uint64_t value = 0;
    for (char c : token) {
        const int nibble = hexValue(c);
        if (nibble < 0) {
            return S2CellID(0);
        }
        value = (value << 4U) | static_cast<uint64_t>(nibble);
    }

    value <<= 4U * (16U - static_cast<uint32_t>(token.size()));
    return S2CellID(value);
}

S2CellID S2CellID::fromFaceLevelPosition(uint8_t face,
                                         uint32_t level,
                                         uint64_t position) {
    if (face > 5 || level > kMaxLevel) {
        return S2CellID(0);
    }

    const uint32_t lsbShift = 2U * (kMaxLevel - level);
    const uint64_t shiftedPosition = position << (lsbShift + 1U);
    const uint64_t id =
        (static_cast<uint64_t>(face) << 61U) | shiftedPosition |
        (uint64_t{1} << lsbShift);
    return S2CellID(id);
}

S2CellID S2CellID::fromQuadtreeTileID(uint8_t face,
                                      uint32_t level,
                                      uint32_t x,
                                      uint32_t y) {
    const uint64_t position = (face & 1U) == 0
        ? encodeHilbert2D(level, x, y)
        : encodeHilbert2D(level, y, x);
    return fromFaceLevelPosition(face, level, position);
}

bool S2CellID::isValid() const noexcept {
    if (id_ == 0 || id_ == std::numeric_limits<uint64_t>::max()) {
        return false;
    }

    const uint8_t face = getFace();
    if (face > 5) {
        return false;
    }

    const uint64_t lowestBit = id_ & (~id_ + 1ULL);
    return lowestBit != 0 && (lowestBit & 0x1555555555555555ULL) != 0;
}

std::string S2CellID::toToken() const {
    if (!isValid()) {
        return {};
    }

    constexpr char kHex[] = "0123456789abcdef";
    std::string token(16, '0');
    uint64_t value = id_;
    for (int i = 15; i >= 0; --i) {
        token[static_cast<size_t>(i)] = kHex[value & 0xFULL];
        value >>= 4U;
    }

    while (!token.empty() && token.back() == '0') {
        token.pop_back();
    }
    return token;
}

int32_t S2CellID::getLevel() const noexcept {
    const uint64_t lowestBit = id_ & (~id_ + 1ULL);
    if (lowestBit == 0) {
        return -1;
    }

    uint32_t shift = 0;
    uint64_t bit = lowestBit;
    while ((bit & 1ULL) == 0) {
        bit >>= 1U;
        ++shift;
    }
    return static_cast<int32_t>(kMaxLevel - shift / 2U);
}

uint8_t S2CellID::getFace() const noexcept {
    return static_cast<uint8_t>(id_ >> 61U);
}

} // namespace earth_engine
