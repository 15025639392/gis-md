#include "ConstrainedDelaunay.h"

#include <CDT.h>

#include <cstdint>
#include <vector>

namespace earth_engine {

// Constrained Delaunay triangulation backed by the vendored artem-ogre/CDT
// library (scaffold/third_party/cdt, MPL-2.0 AND BSD-3-Clause). It is an
// incremental O(n log n) implementation with robust point location (KDTree) and
// Shewchuk predicates, replacing the previous O(n^2) brute-force Bowyer-Watson
// that froze when the VectorFill globe subdivision fed it thousands of points.
std::vector<uint32_t> ConstrainedDelaunay::triangulate(
    const std::vector<glm::dvec2>& points,
    const std::vector<Edge>& constraintEdges,
    ConstrainedDelaunayDiagnostics* diagnostics) {
    const size_t n = points.size();
    if (n < 3) return {};

    CDT::Triangulation<double> cdt;
    std::vector<CDT::V2d<double>> verts;
    verts.reserve(n);
    for (const auto& p : points) verts.emplace_back(p.x, p.y);
    cdt.insertVertices(verts);

    if (!constraintEdges.empty()) {
        std::vector<CDT::Edge> edges;
        edges.reserve(constraintEdges.size());
        for (const auto& e : constraintEdges)
            edges.emplace_back(e.first, e.second);
        cdt.insertEdges(edges);
    }
    cdt.eraseOuterTrianglesAndHoles();

    // Output triangle vertex indices reference cdt.vertices, which preserves
    // the input vertex order (the library never reorders or dedups the buffer),
    // so indices < n map directly back to the caller's point table.
    std::vector<uint32_t> out;
    out.reserve(cdt.triangles.size() * 3);
    for (const auto& t : cdt.triangles) {
        const auto& vv = t.vertices;
        if (vv[0] >= n || vv[1] >= n || vv[2] >= n) continue;
        out.push_back(vv[0]);
        out.push_back(vv[1]);
        out.push_back(vv[2]);
    }
    return out;
}

} // namespace earth_engine
