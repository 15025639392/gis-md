#include "S2CellID.h"

#include "earth_engine/core/math/MathUtils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

namespace earth_engine {

namespace {

constexpr uint32_t kMaxLevel = 30;
constexpr uint8_t kSwapMask = 0x01;
constexpr uint8_t kInvertMask = 0x02;
constexpr uint8_t kPosToIj[4][4] = {
    {0, 1, 3, 2},
    {0, 2, 3, 1},
    {3, 2, 0, 1},
    {3, 1, 0, 2},
};
constexpr uint8_t kPosToOrientation[4] = {
    kSwapMask,
    0,
    0,
    static_cast<uint8_t>(kSwapMask | kInvertMask),
};

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

void decodeS2Position(uint8_t face,
                      uint32_t level,
                      uint64_t position,
                      uint32_t& x,
                      uint32_t& y) {
    uint8_t orientation = face & kSwapMask;
    x = 0;
    y = 0;

    for (uint32_t depth = 1; depth <= level; ++depth) {
        const uint32_t shift = 2U * (level - depth);
        const uint8_t childPosition =
            static_cast<uint8_t>((position >> shift) & 0x3U);
        const uint8_t ij = kPosToIj[orientation][childPosition];
        x = (x << 1U) | (ij >> 1U);
        y = (y << 1U) | (ij & 1U);
        orientation ^= kPosToOrientation[childPosition];
    }
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

uint64_t lowestOnBit(uint64_t id) {
    return id & (~id + 1ULL);
}

double stToUv(double s) {
    if (s >= 0.5) {
        return (4.0 * s * s - 1.0) / 3.0;
    }
    const double oneMinusS = 1.0 - s;
    return (1.0 - 4.0 * oneMinusS * oneMinusS) / 3.0;
}

struct CartesianUnit {
    double x;
    double y;
    double z;
};

CartesianUnit faceUvToXyz(uint8_t face, double u, double v) {
    switch (face) {
        case 0:
            return {1.0, u, v};
        case 1:
            return {-u, 1.0, v};
        case 2:
            return {-u, -v, 1.0};
        case 3:
            return {-1.0, -v, -u};
        case 4:
            return {v, -1.0, -u};
        default:
            return {v, u, -1.0};
    }
}

double latitude(const CartesianUnit& point) {
    return std::atan2(
        point.z,
        std::sqrt(point.x * point.x + point.y * point.y));
}

double longitude(const CartesianUnit& point) {
    return std::atan2(point.y, point.x);
}

Cartographic toCartographic(const CartesianUnit& point) {
    return Cartographic(longitude(point), latitude(point), 0.0);
}

std::pair<double, double> minimalLongitudeInterval(
    std::array<double, 4> longitudes,
    size_t count) {
    if (count == 0) {
        return {-MathUtils::OnePi, MathUtils::OnePi};
    }

    for (size_t i = 0; i < count; ++i) {
        longitudes[i] = MathUtils::negativePiToPi(longitudes[i]);
    }
    std::sort(longitudes.begin(), longitudes.begin() + count);

    size_t largestGapIndex = 0;
    double largestGap = -1.0;
    for (size_t i = 0; i < count; ++i) {
        const double current = longitudes[i];
        const double next = i + 1 < count
            ? longitudes[i + 1]
            : longitudes[0] + MathUtils::TwoPi;
        const double gap = next - current;
        if (gap > largestGap) {
            largestGap = gap;
            largestGapIndex = i;
        }
    }

    const double west =
        longitudes[(largestGapIndex + 1) % count];
    const double east = longitudes[largestGapIndex];
    return {west, east};
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

uint64_t positionWithinLevel(uint64_t id, uint32_t level) {
    const uint32_t lsbShift = 2U * (kMaxLevel - level);
    const uint64_t positionMask = (uint64_t{1} << 61U) - 1U;
    return (id & positionMask) >> (lsbShift + 1U);
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

    const uint64_t lowestBit = lowestOnBit(id_);
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
    const uint64_t lowestBit = lowestOnBit(id_);
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

Cartographic S2CellID::getCenter() const {
    const uint32_t level = static_cast<uint32_t>(getLevel());
    const uint64_t position = positionWithinLevel(id_, level);

    uint32_t x = 0;
    uint32_t y = 0;
    decodeS2Position(getFace(), level, position, x, y);

    const double denominator = static_cast<double>(uint32_t{1} << level);
    const double u =
        stToUv((static_cast<double>(x) + 0.5) / denominator);
    const double v =
        stToUv((static_cast<double>(y) + 0.5) / denominator);
    return toCartographic(faceUvToXyz(getFace(), u, v));
}

std::array<Cartographic, 4> S2CellID::getVertices() const {
    const uint32_t level = static_cast<uint32_t>(getLevel());
    const uint64_t position = positionWithinLevel(id_, level);

    uint32_t x = 0;
    uint32_t y = 0;
    decodeS2Position(getFace(), level, position, x, y);

    const double denominator = static_cast<double>(uint32_t{1} << level);
    const double u0 = stToUv(static_cast<double>(x) / denominator);
    const double u1 = stToUv(static_cast<double>(x + 1U) / denominator);
    const double v0 = stToUv(static_cast<double>(y) / denominator);
    const double v1 = stToUv(static_cast<double>(y + 1U) / denominator);

    const uint8_t face = getFace();
    return {
        toCartographic(faceUvToXyz(face, u0, v0)),
        toCartographic(faceUvToXyz(face, u1, v0)),
        toCartographic(faceUvToXyz(face, u1, v1)),
        toCartographic(faceUvToXyz(face, u0, v1))};
}

S2CellID S2CellID::getParent() const noexcept {
    const uint64_t parentLowestBit = lowestOnBit(id_) << 2U;
    return S2CellID((id_ & (~parentLowestBit + 1ULL)) | parentLowestBit);
}

S2CellID S2CellID::getChild(size_t index) const noexcept {
    const uint64_t lowestBit = lowestOnBit(id_);
    const uint64_t childLowestBit = lowestBit >> 2U;
    const uint64_t childBegin = id_ - lowestBit + childLowestBit;
    return S2CellID(childBegin + static_cast<uint64_t>(index) *
                                    (childLowestBit << 1U));
}

Rectangle S2CellID::computeBoundingRectangle() const {
    const uint8_t face = getFace();
    if (getLevel() == 0) {
        const double poleMinLatitude =
            std::asin(std::sqrt(1.0 / 3.0)) -
            0.5 * std::numeric_limits<double>::epsilon();

        switch (face) {
            case 0:
                return Rectangle(
                    -MathUtils::PiOverFour,
                    -MathUtils::PiOverFour,
                    MathUtils::PiOverFour,
                    MathUtils::PiOverFour);
            case 1:
                return Rectangle(
                    MathUtils::PiOverFour,
                    -MathUtils::PiOverFour,
                    3.0 * MathUtils::PiOverFour,
                    MathUtils::PiOverFour);
            case 2:
                return Rectangle(
                    -MathUtils::OnePi,
                    poleMinLatitude,
                    MathUtils::OnePi,
                    MathUtils::PiOverTwo);
            case 3:
                return Rectangle(
                    3.0 * MathUtils::PiOverFour,
                    -MathUtils::PiOverFour,
                    -3.0 * MathUtils::PiOverFour,
                    MathUtils::PiOverFour);
            case 4:
                return Rectangle(
                    -3.0 * MathUtils::PiOverFour,
                    -MathUtils::PiOverFour,
                    -MathUtils::PiOverFour,
                    MathUtils::PiOverFour);
            default:
                return Rectangle(
                    -MathUtils::OnePi,
                    -MathUtils::PiOverTwo,
                    MathUtils::OnePi,
                    -poleMinLatitude);
        }
    }

    const uint32_t level = static_cast<uint32_t>(getLevel());
    const uint64_t position = positionWithinLevel(id_, level);

    uint32_t x = 0;
    uint32_t y = 0;
    decodeS2Position(face, level, position, x, y);

    const double denominator = static_cast<double>(uint32_t{1} << level);
    const double u0 = stToUv(static_cast<double>(x) / denominator);
    const double u1 = stToUv(static_cast<double>(x + 1U) / denominator);
    const double v0 = stToUv(static_cast<double>(y) / denominator);
    const double v1 = stToUv(static_cast<double>(y + 1U) / denominator);

    const std::array<CartesianUnit, 4> vertices = {
        faceUvToXyz(face, u0, v0),
        faceUvToXyz(face, u1, v0),
        faceUvToXyz(face, u1, v1),
        faceUvToXyz(face, u0, v1)};

    std::array<double, 4> longitudes{};
    size_t longitudeCount = 0;

    double south = latitude(vertices[0]);
    double north = south;
    for (const CartesianUnit& vertex : vertices) {
        const double lat = latitude(vertex);
        south = std::min(south, lat);
        north = std::max(north, lat);
        if (MathUtils::PiOverTwo - std::abs(lat) > MathUtils::Epsilon15) {
            longitudes[longitudeCount++] = longitude(vertex);
        }
    }

    const auto [west, east] =
        minimalLongitudeInterval(longitudes, longitudeCount);
    return Rectangle(west, south, east, north);
}

} // namespace earth_engine
