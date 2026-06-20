#pragma once

#include "Mat4.h"
#include "Vec3.h"

#include <algorithm>
#include <cmath>

namespace earth_engine {

class MathUtils {
public:
    static constexpr double Epsilon1 = 1e-1;
    static constexpr double Epsilon2 = 1e-2;
    static constexpr double Epsilon3 = 1e-3;
    static constexpr double Epsilon4 = 1e-4;
    static constexpr double Epsilon5 = 1e-5;
    static constexpr double Epsilon6 = 1e-6;
    static constexpr double Epsilon7 = 1e-7;
    static constexpr double Epsilon8 = 1e-8;
    static constexpr double Epsilon9 = 1e-9;
    static constexpr double Epsilon10 = 1e-10;
    static constexpr double Epsilon11 = 1e-11;
    static constexpr double Epsilon12 = 1e-12;
    static constexpr double Epsilon13 = 1e-13;
    static constexpr double Epsilon14 = 1e-14;
    static constexpr double Epsilon15 = 1e-15;
    static constexpr double Epsilon16 = 1e-16;
    static constexpr double Epsilon17 = 1e-17;
    static constexpr double Epsilon18 = 1e-18;
    static constexpr double Epsilon19 = 1e-19;
    static constexpr double Epsilon20 = 1e-20;
    static constexpr double Epsilon21 = 1e-21;

    static constexpr double OnePi = 3.14159265358979323846;
    static constexpr double TwoPi = OnePi * 2.0;
    static constexpr double PiOverTwo = OnePi / 2.0;
    static constexpr double PiOverFour = OnePi / 4.0;
    static constexpr double GoldenRatio = 1.61803398874989484;

    static constexpr double degreesToRadians(double angleDegrees) noexcept {
        return angleDegrees * OnePi / 180.0;
    }

    static constexpr double radiansToDegrees(double angleRadians) noexcept {
        return angleRadians * 180.0 / OnePi;
    }

    static constexpr double relativeEpsilonToAbsolute(
        double a,
        double b,
        double relativeEpsilon) noexcept {
        return relativeEpsilon * std::max(std::abs(a), std::abs(b));
    }

    static constexpr bool equalsEpsilon(double left,
                                        double right,
                                        double relativeEpsilon) noexcept {
        return equalsEpsilon(left, right, relativeEpsilon, relativeEpsilon);
    }

    static constexpr bool equalsEpsilon(double left,
                                        double right,
                                        double relativeEpsilon,
                                        double absoluteEpsilon) noexcept {
        const double diff = std::abs(left - right);
        return diff <= absoluteEpsilon ||
               diff <= relativeEpsilonToAbsolute(left, right, relativeEpsilon);
    }

    static bool equalsEpsilon(const Vec3& left,
                              const Vec3& right,
                              double relativeEpsilon) noexcept {
        return equalsEpsilon(left, right, relativeEpsilon, relativeEpsilon);
    }

    static bool equalsEpsilon(const Vec3& left,
                              const Vec3& right,
                              double relativeEpsilon,
                              double absoluteEpsilon) noexcept {
        return equalsEpsilon(left.x(), right.x(), relativeEpsilon, absoluteEpsilon) &&
               equalsEpsilon(left.y(), right.y(), relativeEpsilon, absoluteEpsilon) &&
               equalsEpsilon(left.z(), right.z(), relativeEpsilon, absoluteEpsilon);
    }

    static bool equalsEpsilon(const Mat4& left,
                              const Mat4& right,
                              double relativeEpsilon) noexcept {
        return equalsEpsilon(left, right, relativeEpsilon, relativeEpsilon);
    }

    static bool equalsEpsilon(const Mat4& left,
                              const Mat4& right,
                              double relativeEpsilon,
                              double absoluteEpsilon) noexcept {
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                if (!equalsEpsilon(left(row, col),
                                   right(row, col),
                                   relativeEpsilon,
                                   absoluteEpsilon)) {
                    return false;
                }
            }
        }
        return true;
    }

    static constexpr double sign(double value) noexcept {
        if (value == 0.0 || value != value) {
            return value;
        }
        return value > 0.0 ? 1.0 : -1.0;
    }

    static constexpr double signNotZero(double value) noexcept {
        return value < 0.0 ? -1.0 : 1.0;
    }

    static double mod(double m, double n) noexcept {
        if (sign(m) == sign(n) && std::abs(m) < std::abs(n)) {
            return m;
        }
        return std::fmod(std::fmod(m, n) + n, n);
    }

    static double zeroToTwoPi(double angle) noexcept {
        if (angle >= 0.0 && angle <= TwoPi) {
            return angle;
        }
        const double value = mod(angle, TwoPi);
        if (std::abs(value) < Epsilon14 && std::abs(angle) > Epsilon14) {
            return TwoPi;
        }
        return value;
    }

    static double negativePiToPi(double angle) noexcept {
        if (angle >= -OnePi && angle <= OnePi) {
            return angle;
        }
        return zeroToTwoPi(angle + OnePi) - OnePi;
    }

    static double convertLongitudeRange(double angle) noexcept {
        const double simplified =
            angle - std::floor(angle / TwoPi) * TwoPi;
        if (simplified < -OnePi) {
            return simplified + TwoPi;
        }
        if (simplified >= OnePi) {
            return simplified - TwoPi;
        }
        return simplified;
    }

    static double lerp(double p, double q, double time) noexcept {
        return p * (1.0 - time) + q * time;
    }

    static constexpr double clamp(double value,
                                  double min,
                                  double max) noexcept {
        return std::clamp(value, min, max);
    }

    static double toSNorm(double value,
                          double rangeMaximum = 255.0) noexcept {
        return std::round((clamp(value, -1.0, 1.0) * 0.5 + 0.5) *
                          rangeMaximum);
    }

    static constexpr double fromSNorm(
        double value,
        double rangeMaximum = 255.0) noexcept {
        return (clamp(value, 0.0, rangeMaximum) / rangeMaximum) * 2.0 - 1.0;
    }

    static double roundUp(double value, double tolerance) noexcept {
        const double up = std::ceil(value);
        const double down = std::floor(value);
        if (value - down < tolerance) {
            return down;
        }
        return up;
    }

    static double roundDown(double value, double tolerance) noexcept {
        const double up = std::ceil(value);
        const double down = std::floor(value);
        if (up - value < tolerance) {
            return up;
        }
        return down;
    }
};

} // namespace earth_engine
