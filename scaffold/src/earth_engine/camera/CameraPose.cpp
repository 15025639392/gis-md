#include "CameraPose.h"

#include "../core/geodesy/Ellipsoid.h"
#include "../core/math/Vec3.h"

#include <algorithm>
#include <cmath>

namespace earth_engine {

namespace {

// 参考系局部轴的记号:x=east(右), y=north(前), z=up(上)。
constexpr int kEast = 0;
constexpr int kNorth = 1;
constexpr int kUp = 2;

// |pitch| 逼近 π/2 到这个程度就认为 direction 不再携带 heading 信息。
// cos(pitch) 小于它时,heading 的 atan2 两个分量都在噪声量级。
constexpr double kGimbalCosPitchEpsilon = 1e-9;

}  // namespace

glm::dmat3 CameraPose::enuFrameAt(const glm::dvec3& originEcef) {
    const glm::dvec3 up =
        Ellipsoid::WGS84().geodeticSurfaceNormal(Vec3(originEcef)).raw();
    glm::dvec3 east = glm::cross(glm::dvec3(0.0, 0.0, 1.0), up);
    const double eastLength = glm::length(east);
    // 极点退化:见头文件里"跟已上线读数走"的说明。
    east = eastLength > 1e-9 ? east / eastLength : glm::dvec3(1.0, 0.0, 0.0);
    const glm::dvec3 north = glm::cross(up, east);
    return glm::dmat3(east, north, up);
}

CameraPose CameraPose::fromFrame(const glm::dvec3& originEcef,
                                 const glm::dmat3& frame,
                                 double headingRadians,
                                 double pitchRadians,
                                 double rollRadians,
                                 double rangeMeters) {
    // 参考系局部的内旋 yaw→pitch→roll(绕机体轴依次转),标准 HPR 组合。
    // yaw 绕 up 转 −heading:绕 +up 转 θ 把 +north 送到 (−sinθ, cosθ),而我们要
    // 送到 (sin h, cos h) ⇒ θ = −h。pitch 绕 east 转 +p 把 +north 抬向 +up。
    const glm::dquat q =
        glm::angleAxis(-headingRadians, glm::dvec3(0.0, 0.0, 1.0)) *
        glm::angleAxis(pitchRadians, glm::dvec3(1.0, 0.0, 0.0)) *
        glm::angleAxis(rollRadians, glm::dvec3(0.0, 1.0, 0.0));

    const glm::dvec3 directionLocal = q * glm::dvec3(0.0, 1.0, 0.0);
    const glm::dvec3 upLocal = q * glm::dvec3(0.0, 0.0, 1.0);

    CameraPose pose;
    pose.direction = glm::normalize(frame * directionLocal);
    pose.up = glm::normalize(frame * upLocal);
    pose.eye = originEcef - pose.direction * rangeMeters;
    return pose;
}

void CameraPose::toFrame(const glm::dvec3& originEcef,
                         const glm::dmat3& frame,
                         double& outHeadingRadians,
                         double& outPitchRadians,
                         double& outRollRadians,
                         double& outRangeMeters) const {
    // 正交基的逆 = 转置。
    const glm::dmat3 toLocal = glm::transpose(frame);
    const glm::dvec3 directionLocal = glm::normalize(toLocal * direction);
    const glm::dvec3 upLocal = glm::normalize(toLocal * up);

    const double sinPitch = std::clamp(directionLocal[kUp], -1.0, 1.0);
    outPitchRadians = std::asin(sinPitch);

    const double cosPitch = std::sqrt(std::max(0.0, 1.0 - sinPitch * sinPitch));
    if (cosPitch <= kGimbalCosPitchEpsilon) {
        // 万向节:视线沿天顶/天底,绕它转不改 direction ⇒ heading 只能由 up 定。
        // 正俯视时屏幕"上"指向的方位角即 heading(与 headingFromFrame 的兜底同源);
        // 正仰视时 up 的水平分量指向视线背后,故取反。
        const double sign = sinPitch >= 0.0 ? -1.0 : 1.0;
        outHeadingRadians =
            std::atan2(sign * upLocal[kEast], sign * upLocal[kNorth]);
        outRollRadians = 0.0;
    } else {
        outHeadingRadians =
            std::atan2(directionLocal[kEast], directionLocal[kNorth]);
        // roll = up 相对"零 roll 时的 up"绕视线的有符号夹角。
        const glm::dquat qNoRoll =
            glm::angleAxis(-outHeadingRadians, glm::dvec3(0.0, 0.0, 1.0)) *
            glm::angleAxis(outPitchRadians, glm::dvec3(1.0, 0.0, 0.0));
        const glm::dvec3 upNoRoll = qNoRoll * glm::dvec3(0.0, 0.0, 1.0);
        outRollRadians =
            std::atan2(glm::dot(glm::cross(upNoRoll, upLocal), directionLocal),
                       glm::dot(upNoRoll, upLocal));
    }

    if (outHeadingRadians < 0.0) {
        outHeadingRadians += 2.0 * glm::pi<double>();
    }
    outRangeMeters = glm::length(originEcef - eye);
}

} // namespace earth_engine
