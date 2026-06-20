#include "S2CellBoundingVolume.h"

#include "Cartographic.h"
#include "Ellipsoid.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace earth_engine {
namespace {

std::array<Plane, 6> computeBoundingPlanes(
    const S2CellID& cellID,
    const Vec3& center,
    double minimumHeight,
    double maximumHeight,
    const Ellipsoid& ellipsoid) {
    std::array<Plane, 6> planes;

    const Vec3 centerSurfaceNormal = ellipsoid.geodeticSurfaceNormal(center);
    Cartographic topCartographic = ellipsoid.cartesianToCartographic(center);
    topCartographic = Cartographic::fromRadians(
        topCartographic.longitude(),
        topCartographic.latitude(),
        maximumHeight);
    const Vec3 top = ellipsoid.cartographicToCartesian(topCartographic);
    planes[0] = Plane(top, centerSurfaceNormal);
    const Plane& topPlane = planes[0];

    std::array<Cartographic, 4> verticesCartographic = cellID.getVertices();
    std::array<Vec3, 4> verticesCartesian;
    double maxDistance = 0.0;
    for (std::size_t i = 0; i < verticesCartographic.size(); ++i) {
        verticesCartographic[i] = Cartographic::fromRadians(
            verticesCartographic[i].longitude(),
            verticesCartographic[i].latitude(),
            minimumHeight);
        verticesCartesian[i] =
            ellipsoid.cartographicToCartesian(verticesCartographic[i]);
        const double distance = topPlane.getPointDistance(verticesCartesian[i]);
        if (distance < maxDistance) {
            maxDistance = distance;
        }
    }

    planes[1] = Plane(
        -topPlane.getNormal(),
        -topPlane.getDistance() + maxDistance);

    for (std::size_t i = 0; i < 4; ++i) {
        const Vec3& vertex = verticesCartesian[i];
        const Vec3& adjacentVertex = verticesCartesian[(i + 1) % 4];
        const Vec3 geodeticNormal = ellipsoid.geodeticSurfaceNormal(vertex);
        const Vec3 side = adjacentVertex - vertex;
        const Vec3 sideNormal = side.cross(geodeticNormal).normalized();
        planes[i + 2] = Plane(vertex, sideNormal);
    }

    return planes;
}

Vec3 computeIntersection(const Plane& p0,
                         const Plane& p1,
                         const Plane& p2) {
    const glm::dvec3& n0 = p0.getNormal().raw();
    const glm::dvec3& n1 = p1.getNormal().raw();
    const glm::dvec3& n2 = p2.getNormal().raw();

    glm::dmat3x3 matrix(
        glm::dvec3(n0.x, n1.x, n2.x),
        glm::dvec3(n0.y, n1.y, n2.y),
        glm::dvec3(n0.z, n1.z, n2.z));

    const double determinant = glm::determinant(matrix);
    const glm::dvec3 x0 = n0 * -p0.getDistance();
    const glm::dvec3 x1 = n1 * -p1.getDistance();
    const glm::dvec3 x2 = n2 * -p2.getDistance();

    const glm::dvec3 f0 = glm::cross(n1, n2) * glm::dot(x0, n0);
    const glm::dvec3 f1 = glm::cross(n2, n0) * glm::dot(x1, n1);
    const glm::dvec3 f2 = glm::cross(n0, n1) * glm::dot(x2, n2);
    return Vec3((f0 + f1 + f2) / determinant);
}

std::array<Vec3, 8> computeVertices(
    const std::array<Plane, 6>& boundingPlanes) {
    std::array<Vec3, 8> vertices;
    for (std::size_t i = 0; i < 4; ++i) {
        vertices[i] = computeIntersection(
            boundingPlanes[0],
            boundingPlanes[2 + ((i + 3) % 4)],
            boundingPlanes[2 + (i % 4)]);
        vertices[i + 4] = computeIntersection(
            boundingPlanes[1],
            boundingPlanes[2 + ((i + 3) % 4)],
            boundingPlanes[2 + (i % 4)]);
    }
    return vertices;
}

std::array<Vec3, 4> getPlaneVertices(
    const std::array<Vec3, 8>& vertices,
    std::size_t index) {
    if (index <= 1) {
        const std::size_t start = index * 4;
        return {
            vertices[start],
            vertices[start + 1],
            vertices[start + 2],
            vertices[start + 3]};
    }

    const std::size_t i = index - 2;
    return {
        vertices[i % 4],
        vertices[(i + 1) % 4],
        vertices[4 + ((i + 1) % 4)],
        vertices[4 + i]};
}

std::array<Vec3, 4> computeEdgeNormals(
    const Plane& plane,
    const std::array<Vec3, 4>& vertices,
    bool invert) {
    std::array<Vec3, 4> result = {
        plane.getNormal().cross(vertices[1] - vertices[0]).normalized(),
        plane.getNormal().cross(vertices[2] - vertices[1]).normalized(),
        plane.getNormal().cross(vertices[3] - vertices[2]).normalized(),
        plane.getNormal().cross(vertices[0] - vertices[3]).normalized()};

    if (invert) {
        for (Vec3& normal : result) {
            normal = -normal;
        }
    }
    return result;
}

Vec3 closestPointLineSegment(const Vec3& p, const Vec3& l0, const Vec3& l1) {
    const Vec3 d = l1 - l0;
    double t = d.dot(p - l0);
    if (t <= 0.0) return l0;

    const double dMag = d.dot(d);
    if (t >= dMag) return l1;

    t /= dMag;
    return l0 * (1.0 - t) + l1 * t;
}

Vec3 closestPointPolygon(const Vec3& p,
                         const std::array<Vec3, 4>& vertices,
                         const std::array<Vec3, 4>& edgeNormals) {
    double minDistance = std::numeric_limits<double>::max();
    Vec3 closestPoint = p;

    for (std::size_t i = 0; i < vertices.size(); ++i) {
        Plane edgePlane(vertices[i], edgeNormals[i]);
        if (edgePlane.getPointDistance(p) < 0.0) {
            continue;
        }

        Vec3 closestPointOnEdge =
            closestPointLineSegment(p, vertices[i], vertices[(i + 1) % 4]);
        const double distance = p.distanceTo(closestPointOnEdge);
        if (distance < minDistance) {
            minDistance = distance;
            closestPoint = closestPointOnEdge;
        }
    }

    return closestPoint;
}

} // namespace

S2CellBoundingVolume::S2CellBoundingVolume(const S2CellID& cellID,
                                           double minimumHeight,
                                           double maximumHeight)
    : cellID_(cellID),
      minimumHeight_(minimumHeight),
      maximumHeight_(maximumHeight) {
    if (!cellID_.isValid()) {
        return;
    }

    Cartographic centerCartographic = cellID_.getCenter();
    centerCartographic = Cartographic::fromRadians(
        centerCartographic.longitude(),
        centerCartographic.latitude(),
        (minimumHeight_ + maximumHeight_) * 0.5);
    center_ = Ellipsoid::WGS84().cartographicToCartesian(centerCartographic);
    boundingPlanes_ = computeBoundingPlanes(
        cellID_,
        center_,
        minimumHeight_,
        maximumHeight_,
        Ellipsoid::WGS84());
    vertices_ = computeVertices(boundingPlanes_);
}

double S2CellBoundingVolume::computeDistanceSquaredToPosition(
    const Vec3& position) const noexcept {
    std::size_t numSelectedPlanes = 0;
    std::array<std::size_t, 6> selectedPlaneIndices{};

    if (boundingPlanes_[0].getPointDistance(position) > 0.0) {
        selectedPlaneIndices[numSelectedPlanes++] = 0;
    } else if (boundingPlanes_[1].getPointDistance(position) > 0.0) {
        selectedPlaneIndices[numSelectedPlanes++] = 1;
    }

    for (std::size_t i = 0; i < 4; ++i) {
        const std::size_t sidePlaneIndex = 2 + i;
        if (boundingPlanes_[sidePlaneIndex].getPointDistance(position) > 0.0) {
            selectedPlaneIndices[numSelectedPlanes++] = sidePlaneIndex;
        }
    }

    if (numSelectedPlanes == 0) {
        return 0.0;
    }

    if (numSelectedPlanes == 1) {
        const std::size_t planeIndex = selectedPlaneIndices[0];
        const Plane& selectedPlane = boundingPlanes_[planeIndex];
        const std::array<Vec3, 4> vertices =
            getPlaneVertices(vertices_, planeIndex);
        const std::array<Vec3, 4> edgeNormals =
            computeEdgeNormals(selectedPlane, vertices, planeIndex == 0);
        const Vec3 facePoint = closestPointPolygon(
            selectedPlane.projectPointOntoPlane(position),
            vertices,
            edgeNormals);
        return (facePoint - position).lengthSquared();
    }

    if (numSelectedPlanes == 2) {
        if (selectedPlaneIndices[0] == 0) {
            const Vec3 facePoint = closestPointLineSegment(
                position,
                vertices_[4 * selectedPlaneIndices[0] +
                          (selectedPlaneIndices[1] - 2)],
                vertices_[4 * selectedPlaneIndices[0] +
                          ((selectedPlaneIndices[1] - 2 + 1) % 4)]);
            return (facePoint - position).lengthSquared();
        }

        double minimumDistanceSquared = std::numeric_limits<double>::max();
        for (std::size_t i = 0; i < 2; ++i) {
            const std::size_t planeIndex = selectedPlaneIndices[i];
            const Plane& selectedPlane = boundingPlanes_[planeIndex];
            const std::array<Vec3, 4> vertices =
                getPlaneVertices(vertices_, planeIndex);
            const std::array<Vec3, 4> edgeNormals =
                computeEdgeNormals(selectedPlane, vertices, planeIndex == 0);
            const Vec3 facePoint = closestPointPolygon(
                selectedPlane.projectPointOntoPlane(position),
                vertices,
                edgeNormals);
            minimumDistanceSquared = std::min(
                minimumDistanceSquared,
                (facePoint - position).lengthSquared());
        }
        return minimumDistanceSquared;
    }

    if (numSelectedPlanes > 3) {
        const std::array<Vec3, 4> vertices = getPlaneVertices(vertices_, 1);
        const Vec3 facePoint = closestPointPolygon(
            boundingPlanes_[1].projectPointOntoPlane(position),
            vertices,
            computeEdgeNormals(boundingPlanes_[1], vertices, false));
        return (facePoint - position).lengthSquared();
    }

    const std::size_t skip =
        selectedPlaneIndices[1] == 2 && selectedPlaneIndices[2] == 5 ? 0 : 1;

    if (selectedPlaneIndices[0] == 0) {
        return (position -
                vertices_[(selectedPlaneIndices[1] - 2 + skip) % 4])
            .lengthSquared();
    }

    return (position -
            vertices_[4 + ((selectedPlaneIndices[1] - 2 + skip) % 4)])
        .lengthSquared();
}

int S2CellBoundingVolume::intersectPlane(const Plane& plane) const noexcept {
    std::size_t plusCount = 0;
    std::size_t negCount = 0;

    for (const Vec3& vertex : vertices_) {
        if (plane.getPointDistance(vertex) < 0.0) {
            ++negCount;
        } else {
            ++plusCount;
        }
    }

    if (plusCount == vertices_.size()) {
        return 1;
    }
    if (negCount == vertices_.size()) {
        return -1;
    }
    return 0;
}

} // namespace earth_engine
