#include "Mat4.h"
#include "Vec3.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace earth_engine {

Mat4::Mat4() : m_(1.0) {}

double Mat4::operator()(int row, int col) const { return m_[col][row]; }
double& Mat4::operator()(int row, int col) { return m_[col][row]; }

Mat4 Mat4::operator*(const Mat4& rhs) const { return Mat4(m_ * rhs.m_); }

Vec3 Mat4::operator*(const Vec3& v) const {
    glm::dvec4 r = m_ * glm::dvec4(v.raw(), 1.0);
    return Vec3(glm::dvec3(r) / r.w);
}

Mat4 Mat4::inverse() const { return Mat4(glm::inverse(m_)); }
Mat4 Mat4::transpose() const { return Mat4(glm::transpose(m_)); }

Mat4 Mat4::identity() { return Mat4(); }
Mat4 Mat4::translation(const Vec3& t) { return Mat4(glm::translate(glm::dmat4(1.0), t.raw())); }
Mat4 Mat4::scale(const Vec3& s) { return Mat4(glm::scale(glm::dmat4(1.0), s.raw())); }
Mat4 Mat4::rotationX(double rad) { return Mat4(glm::rotate(glm::dmat4(1.0), rad, glm::dvec3(1, 0, 0))); }
Mat4 Mat4::rotationY(double rad) { return Mat4(glm::rotate(glm::dmat4(1.0), rad, glm::dvec3(0, 1, 0))); }
Mat4 Mat4::rotationZ(double rad) { return Mat4(glm::rotate(glm::dmat4(1.0), rad, glm::dvec3(0, 0, 1))); }

bool Mat4::operator==(const Mat4& rhs) const { return m_ == rhs.m_; }
bool Mat4::operator!=(const Mat4& rhs) const { return m_ != rhs.m_; }

std::ostream& operator<<(std::ostream& os, const Mat4& m) {
    return os << "Mat4(\n"
              << "  " << m(0,0) << " " << m(0,1) << " " << m(0,2) << " " << m(0,3) << "\n"
              << "  " << m(1,0) << " " << m(1,1) << " " << m(1,2) << " " << m(1,3) << "\n"
              << "  " << m(2,0) << " " << m(2,1) << " " << m(2,2) << " " << m(2,3) << "\n"
              << "  " << m(3,0) << " " << m(3,1) << " " << m(3,2) << " " << m(3,3) << ")";
}

} // namespace earth_engine
