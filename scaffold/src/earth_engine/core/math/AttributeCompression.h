#pragma once

#include "MathUtils.h"

#include <glm/ext/vector_double3.hpp>
#include <glm/geometric.hpp>

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace earth_engine {

class AttributeCompression {
public:
    template <typename T,
              typename = std::enable_if_t<std::is_unsigned<T>::value>>
    static glm::dvec3 octDecodeInRange(T x, T y, T rangeMax) noexcept {
        glm::dvec3 result;
        result.x = MathUtils::fromSNorm(static_cast<double>(x),
                                        static_cast<double>(rangeMax));
        result.y = MathUtils::fromSNorm(static_cast<double>(y),
                                        static_cast<double>(rangeMax));
        result.z = 1.0 - (std::abs(result.x) + std::abs(result.y));

        if (result.z < 0.0) {
            const double oldX = result.x;
            result.x = (1.0 - std::abs(result.y)) *
                       MathUtils::signNotZero(oldX);
            result.y = (1.0 - std::abs(oldX)) *
                       MathUtils::signNotZero(result.y);
        }

        return glm::normalize(result);
    }

    static glm::dvec3 octDecode(uint8_t x, uint8_t y) noexcept {
        constexpr uint8_t kRangeMax = 255;
        return octDecodeInRange(x, y, kRangeMax);
    }

    static glm::dvec3 decodeRGB565(uint16_t value) noexcept {
        constexpr uint16_t kMask5 = (1u << 5u) - 1u;
        constexpr uint16_t kMask6 = (1u << 6u) - 1u;
        constexpr double kNormalize5 = 1.0 / 31.0;
        constexpr double kNormalize6 = 1.0 / 63.0;

        const uint16_t red = static_cast<uint16_t>(value >> 11u);
        const uint16_t green = static_cast<uint16_t>((value >> 5u) & kMask6);
        const uint16_t blue = value & kMask5;

        return glm::dvec3(red * kNormalize5,
                          green * kNormalize6,
                          blue * kNormalize5);
    }
};

} // namespace earth_engine
