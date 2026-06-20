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
};

} // namespace earth_engine
