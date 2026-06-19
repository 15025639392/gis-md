#include "IntersectionTests.h"

#include "AxisAlignedBox.h"
#include "BoundingSphere.h"
#include "OrientedBoundingBox.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <glm/ext/matrix_double3x3.hpp>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

namespace earth_engine {

namespace {
double component(const Vec3& v, int index) noexcept {
    switch (index) {
    case 0:
        return v.x();
    case 1:
        return v.y();
    default:
        return v.z();
    }
}

bool solveQuadratic(double a,
                    double b,
                    double c,
                    double& root0,
                    double& root1) {
    const double det = b * b - 4.0 * a * c;
    if (det < 0.0) {
        return false;
    }
    if (det > 0.0) {
        const double denom = 1.0 / (2.0 * a);
        const double disc = std::sqrt(det);
        root0 = (-b + disc) * denom;
        root1 = (-b - disc) * denom;

        if (root1 < root0) {
            std::swap(root1, root0);
        }
        return true;
    }

    const double root = -b / (2.0 * a);
    if (root == 0.0) {
        return false;
    }

    root0 = root;
    root1 = root;
    return true;
}
} // namespace

std::optional<Vec3> IntersectionTests::rayPlane(const Ray& ray, const Plane& plane) noexcept {
    constexpr double epsilon15 = 1e-15;
    const Vec3& normal = plane.getNormal();
    const double denominator = normal.dot(ray.direction());

    if (std::abs(denominator) < epsilon15) {
        return std::nullopt;
    }

    const double t = (-plane.getDistance() - normal.dot(ray.origin())) / denominator;
    if (t < 0.0) {
        return std::nullopt;
    }

    return ray.origin() + ray.direction() * t;
}

std::optional<RayEllipsoidIntersectionInterval>
IntersectionTests::rayEllipsoid(const Ray& ray, const Vec3& radii) noexcept {
    if (radii.x() == 0.0 || radii.y() == 0.0 || radii.z() == 0.0) {
        return std::nullopt;
    }

    const Vec3 inverseRadii(1.0 / radii.x(), 1.0 / radii.y(), 1.0 / radii.z());
    const Vec3 q(ray.origin().x() * inverseRadii.x(),
                 ray.origin().y() * inverseRadii.y(),
                 ray.origin().z() * inverseRadii.z());
    const Vec3 w(ray.direction().x() * inverseRadii.x(),
                 ray.direction().y() * inverseRadii.y(),
                 ray.direction().z() * inverseRadii.z());

    const double q2 = q.dot(q);
    const double qw = q.dot(w);
    const double w2 = w.dot(w);
    if (w2 <= 0.0) {
        return std::nullopt;
    }

    if (q2 > 1.0) {
        if (qw >= 0.0) {
            return std::nullopt;
        }

        const double qw2 = qw * qw;
        const double difference = q2 - 1.0;
        const double product = w2 * difference;

        if (qw2 < product) {
            return std::nullopt;
        }
        if (qw2 > product) {
            const double discriminant = qw2 - product;
            const double temp = -qw + std::sqrt(discriminant);
            const double root0 = temp / w2;
            const double root1 = difference / temp;
            return root0 < root1
                ? RayEllipsoidIntersectionInterval{root0, root1}
                : RayEllipsoidIntersectionInterval{root1, root0};
        }

        const double root = std::sqrt(difference / w2);
        return RayEllipsoidIntersectionInterval{root, root};
    }

    if (q2 < 1.0) {
        const double difference = q2 - 1.0;
        const double product = w2 * difference;
        const double discriminant = qw * qw - product;
        const double temp = -qw + std::sqrt(discriminant);
        return RayEllipsoidIntersectionInterval{0.0, temp / w2};
    }

    if (qw < 0.0) {
        return RayEllipsoidIntersectionInterval{0.0, -qw / w2};
    }

    return std::nullopt;
}

std::optional<Vec3> IntersectionTests::rayTriangle(const Ray& ray,
                                                   const Vec3& v0,
                                                   const Vec3& v1,
                                                   const Vec3& v2,
                                                   bool cullBackFaces) {
    const std::optional<double> t =
        rayTriangleParametric(ray, v0, v1, v2, cullBackFaces);
    if (t && *t >= 0.0) {
        return ray.pointAt(*t);
    }
    return std::nullopt;
}

std::optional<double> IntersectionTests::rayTriangleParametric(const Ray& ray,
                                                               const Vec3& p0,
                                                               const Vec3& p1,
                                                               const Vec3& p2,
                                                               bool cullBackFaces) {
    constexpr double epsilon8 = 1e-8;
    const Vec3& origin = ray.origin();
    const Vec3& direction = ray.direction();

    const Vec3 edge0 = p1 - p0;
    const Vec3 edge1 = p2 - p0;

    const Vec3 p = direction.cross(edge1);
    const double det = edge0.dot(p);
    if (cullBackFaces) {
        if (det < epsilon8) {
            return std::nullopt;
        }

        const Vec3 tvec = origin - p0;
        const double u = tvec.dot(p);
        if (u < 0.0 || u > det) {
            return std::nullopt;
        }

        const Vec3 q = tvec.cross(edge0);
        const double v = direction.dot(q);
        if (v < 0.0 || u + v > det) {
            return std::nullopt;
        }

        return edge1.dot(q) / det;
    }

    if (std::abs(det) < epsilon8) {
        return std::nullopt;
    }

    const double invDet = 1.0 / det;
    const Vec3 tvec = origin - p0;
    const double u = tvec.dot(p) * invDet;
    if (u < 0.0 || u > 1.0) {
        return std::nullopt;
    }

    const Vec3 q = tvec.cross(edge0);
    const double v = direction.dot(q) * invDet;
    if (v < 0.0 || u + v > 1.0) {
        return std::nullopt;
    }

    return edge1.dot(q) * invDet;
}

std::optional<Vec3> IntersectionTests::rayAABB(const Ray& ray,
                                               const AxisAlignedBox& aabb) {
    const std::optional<double> t = rayAABBParametric(ray, aabb);
    if (t && *t >= 0.0) {
        return ray.pointAt(*t);
    }
    return std::nullopt;
}

std::optional<double> IntersectionTests::rayAABBParametric(
    const Ray& ray,
    const AxisAlignedBox& aabb) {
    constexpr double epsilon6 = 1e-6;
    const Vec3& dir = ray.direction();
    const Vec3& origin = ray.origin();
    const Vec3 minimum(aabb.minimumX(), aabb.minimumY(), aabb.minimumZ());
    const Vec3 maximum(aabb.maximumX(), aabb.maximumY(), aabb.maximumZ());

    double greatestMin = -std::numeric_limits<double>::max();
    double smallestMax = std::numeric_limits<double>::max();
    double tmin = greatestMin;
    double tmax = smallestMax;

    for (int i = 0; i < 3; ++i) {
        if (std::abs(component(dir, i)) < epsilon6) {
            continue;
        }

        tmin = (component(minimum, i) - component(origin, i)) / component(dir, i);
        tmax = (component(maximum, i) - component(origin, i)) / component(dir, i);

        if (tmin > tmax) {
            std::swap(tmin, tmax);
        }
        greatestMin = std::max(tmin, greatestMin);
        smallestMax = std::min(tmax, smallestMax);
    }

    if (smallestMax < 0.0 || greatestMin > smallestMax) {
        return std::nullopt;
    }
    return greatestMin < 0.0 ? smallestMax : greatestMin;
}

std::optional<Vec3> IntersectionTests::rayOBB(const Ray& ray,
                                              const OrientedBoundingBox& obb) {
    const std::optional<double> t = rayOBBParametric(ray, obb);
    if (t && *t >= 0.0) {
        return ray.pointAt(*t);
    }
    return std::nullopt;
}

std::optional<double> IntersectionTests::rayOBBParametric(
    const Ray& ray,
    const OrientedBoundingBox& obb) {
    const Vec3 halfLengths = obb.getLengths() * 0.5;
    const glm::dmat3 halfAxes(obb.getHalfAxis(0).raw(),
                              obb.getHalfAxis(1).raw(),
                              obb.getHalfAxis(2).raw());
    const glm::dmat3 rotationOnly(
        halfAxes[0] / halfLengths.x(),
        halfAxes[1] / halfLengths.y(),
        halfAxes[2] / halfLengths.z());
    const glm::dmat3 inverseRotation = glm::transpose(rotationOnly);

    const Vec3 relativeOrigin = ray.origin() - obb.getCenter();
    const Vec3 rayOrigin(inverseRotation * relativeOrigin.raw());
    const Vec3 rayDirection(inverseRotation * ray.direction().raw());

    const AxisAlignedBox aabb(-halfLengths.x(),
                              -halfLengths.y(),
                              -halfLengths.z(),
                              halfLengths.x(),
                              halfLengths.y(),
                              halfLengths.z());
    return rayAABBParametric(Ray(rayOrigin, rayDirection), aabb);
}

std::optional<Vec3> IntersectionTests::raySphere(const Ray& ray,
                                                 const BoundingSphere& sphere) {
    const std::optional<double> t = raySphereParametric(ray, sphere);
    if (t && *t >= 0.0) {
        return ray.pointAt(*t);
    }
    return std::nullopt;
}

std::optional<double> IntersectionTests::raySphereParametric(
    const Ray& ray,
    const BoundingSphere& sphere) {
    const Vec3 diff = ray.origin() - sphere.getCenter();
    const double radiusSquared = sphere.getRadius() * sphere.getRadius();
    const double b = 2.0 * ray.direction().dot(diff);
    const double c = diff.dot(diff) - radiusSquared;

    double t0 = 0.0;
    double t1 = 0.0;
    if (solveQuadratic(1.0, b, c, t0, t1)) {
        return t0 < 0.0 ? t1 : t0;
    }
    return std::nullopt;
}

bool IntersectionTests::pointInTriangle(const glm::dvec2& point,
                                        const glm::dvec2& triangleVertA,
                                        const glm::dvec2& triangleVertB,
                                        const glm::dvec2& triangleVertC) noexcept {
    const glm::dvec2 ab = triangleVertB - triangleVertA;
    const glm::dvec2 bc = triangleVertC - triangleVertB;
    const glm::dvec2 ca = triangleVertA - triangleVertC;

    const glm::dvec2 abPerp(-ab.y, ab.x);
    const glm::dvec2 bcPerp(-bc.y, bc.x);
    const glm::dvec2 caPerp(-ca.y, ca.x);

    const glm::dvec2 av = point - triangleVertA;
    const glm::dvec2 cv = point - triangleVertC;

    const double vProjAbPerp = glm::dot(av, abPerp);
    const double vProjBcPerp = glm::dot(cv, bcPerp);
    const double vProjCaPerp = glm::dot(cv, caPerp);

    return (vProjAbPerp >= 0.0 && vProjCaPerp >= 0.0 &&
            vProjBcPerp >= 0.0) ||
           (vProjAbPerp <= 0.0 && vProjCaPerp <= 0.0 &&
            vProjBcPerp <= 0.0);
}

bool IntersectionTests::pointInTriangle(const Vec3& point,
                                        const Vec3& triangleVertA,
                                        const Vec3& triangleVertB,
                                        const Vec3& triangleVertC) noexcept {
    Vec3 unused;
    return pointInTriangle(point,
                           triangleVertA,
                           triangleVertB,
                           triangleVertC,
                           unused);
}

bool IntersectionTests::pointInTriangle(const Vec3& point,
                                        const Vec3& triangleVertA,
                                        const Vec3& triangleVertB,
                                        const Vec3& triangleVertC,
                                        Vec3& barycentricCoordinates) noexcept {
    constexpr double epsilon8 = 1e-8;
    const Vec3 ab = triangleVertB - triangleVertA;
    const Vec3 bc = triangleVertC - triangleVertB;

    const Vec3 triangleNormal = ab.cross(bc);
    const double lengthSquared = triangleNormal.lengthSquared();
    if (lengthSquared < epsilon8) {
        return false;
    }

    const double triangleAreaInv = 1.0 / std::sqrt(lengthSquared);

    const Vec3 ap = point - triangleVertA;
    const double triangleABPRatio =
        ab.cross(ap).length() * triangleAreaInv;
    if (triangleABPRatio > 1.0) {
        return false;
    }

    const Vec3 bp = point - triangleVertB;
    const double triangleBCPRatio =
        bc.cross(bp).length() * triangleAreaInv;
    if (triangleBCPRatio > 1.0) {
        return false;
    }

    const double triangleCAPRatio =
        1.0 - triangleABPRatio - triangleBCPRatio;
    if (triangleCAPRatio < 0.0) {
        return false;
    }

    barycentricCoordinates.x() = triangleBCPRatio;
    barycentricCoordinates.y() = triangleCAPRatio;
    barycentricCoordinates.z() = triangleABPRatio;

    return true;
}

} // namespace earth_engine
