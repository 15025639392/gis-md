#pragma once

#include <glm/glm.hpp>
#include <ostream>

namespace earth_engine {

/// 三维笛卡尔向量（double precision，内部存储）。
/// 封装 glm::dvec3，在需要与 GLM 互操作时通过 raw() 访问底层。
class Vec3 {
public:
    Vec3() : v_(0.0, 0.0, 0.0) {}
    Vec3(double x, double y, double z) : v_(x, y, z) {}
    explicit Vec3(const glm::dvec3& v) : v_(v) {}

    double x() const { return v_.x; }
    double y() const { return v_.y; }
    double z() const { return v_.z; }
    double& x() { return v_.x; }
    double& y() { return v_.y; }
    double& z() { return v_.z; }

    /// 直接访问底层 glm::dvec3（用于传递给 GLM 函数）
    const glm::dvec3& raw() const { return v_; }
    glm::dvec3& raw() { return v_; }

    // 算术
    Vec3 operator+(const Vec3& rhs) const { return Vec3(v_ + rhs.v_); }
    Vec3 operator-(const Vec3& rhs) const { return Vec3(v_ - rhs.v_); }
    Vec3 operator*(double s) const { return Vec3(v_ * s); }
    Vec3 operator/(double s) const { return Vec3(v_ / s); }
    Vec3& operator+=(const Vec3& rhs) { v_ += rhs.v_; return *this; }
    Vec3& operator-=(const Vec3& rhs) { v_ -= rhs.v_; return *this; }

    Vec3 operator-() const { return Vec3(-v_); }

    // 向量操作
    double length() const;
    double lengthSquared() const;
    Vec3 normalized() const;
    double dot(const Vec3& rhs) const;
    Vec3 cross(const Vec3& rhs) const;
    double distanceTo(const Vec3& rhs) const;

    bool operator==(const Vec3& rhs) const { return v_ == rhs.v_; }
    bool operator!=(const Vec3& rhs) const { return v_ != rhs.v_; }

    static Vec3 zero() { return Vec3(0, 0, 0); }
    static Vec3 unitX() { return Vec3(1, 0, 0); }
    static Vec3 unitY() { return Vec3(0, 1, 0); }
    static Vec3 unitZ() { return Vec3(0, 0, 1); }

private:
    glm::dvec3 v_;
};

Vec3 operator*(double s, const Vec3& v);

std::ostream& operator<<(std::ostream& os, const Vec3& v);

} // namespace earth_engine
