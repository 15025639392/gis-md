#pragma once

#include <glm/glm.hpp>
#include <ostream>
#include <array>

namespace earth_engine {

class Vec3;

/// 4×4 列主序矩阵（double precision）。
/// 封装 glm::dmat4。
class Mat4 {
public:
    Mat4();  // 单位矩阵
    explicit Mat4(const glm::dmat4& m) : m_(m) {}

    const glm::dmat4& raw() const { return m_; }
    glm::dmat4& raw() { return m_; }

    // 元素访问（列优先：col[0..3], row[0..3]）
    double operator()(int row, int col) const;
    double& operator()(int row, int col);

    Mat4 operator*(const Mat4& rhs) const;
    Vec3 operator*(const Vec3& v) const;  // 齐次乘法（w=1）
    Vec3 transformPoint(const Vec3& point) const;
    Vec3 transformVector(const Vec3& vector) const;

    Mat4 inverse() const;
    Mat4 transpose() const;

    static Mat4 identity();
    static Mat4 translation(const Vec3& t);
    static Mat4 scale(const Vec3& s);
    static Mat4 rotationX(double radians);
    static Mat4 rotationY(double radians);
    static Mat4 rotationZ(double radians);

    bool operator==(const Mat4& rhs) const;
    bool operator!=(const Mat4& rhs) const;

    const double* data() const { return &m_[0][0]; }

private:
    glm::dmat4 m_;
};

std::ostream& operator<<(std::ostream& os, const Mat4& m);

} // namespace earth_engine
