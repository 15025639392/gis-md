#include "ConstrainedDelaunay.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <cassert>
#include <cmath>
#include <deque>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace earth_engine {

namespace {

using Vec2 = glm::dvec2;
struct Tri { uint32_t v[3]; };

// ---- predicates(double;lng/lat 弧度尺度条件数好,足够)----

// >0: c 在有向直线 a→b 左侧(a,b,c 逆时针)。
double orient2d(const Vec2& a, const Vec2& b, const Vec2& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// a,b,c 逆时针时:>0 表示 d 严格在 abc 外接圆内。
double inCircleDet(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d) {
    const double ax = a.x - d.x, ay = a.y - d.y;
    const double bx = b.x - d.x, by = b.y - d.y;
    const double cx = c.x - d.x, cy = c.y - d.y;
    return (ax * ax + ay * ay) * (bx * cy - cx * by) -
           (bx * bx + by * by) * (ax * cy - cx * ay) +
           (cx * cx + cy * cy) * (ax * by - bx * ay);
}

// winding 无关:d 是否严格在 abc 外接圆内。
bool inCircleUnsigned(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d) {
    const double o = orient2d(a, b, c);
    if (o == 0.0) return false;  // 退化三角形无外接圆
    const double det = inCircleDet(a, b, c, d);
    return o > 0.0 ? det > 0.0 : det < 0.0;
}

// 两线段是否"真"相交(内部相交,不含端点接触/共线)。
bool segmentsProperlyCross(const Vec2& p1, const Vec2& p2, const Vec2& p3,
                           const Vec2& p4) {
    const double d1 = orient2d(p3, p4, p1);
    const double d2 = orient2d(p3, p4, p2);
    const double d3 = orient2d(p1, p2, p3);
    const double d4 = orient2d(p1, p2, p4);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
           ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

uint64_t edgeKey(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | b;
}

// CDT 实现体(持有点集与三角形,便于递归/辅助共享状态)。
class Cdt {
public:
    Cdt(const std::vector<Vec2>& points,
        const std::vector<ConstrainedDelaunay::Edge>& constraints,
        ConstrainedDelaunayDiagnostics* diagnostics)
        : pts_(points), constraints_(constraints), diagnostics_(diagnostics) {
        // The super triangle adds exactly three points. A planar triangulation
        // of V vertices has fewer than 2V triangles; reserving that bound
        // removes deterministic whole-table reallocations without changing
        // insertion order, predicates, traversal, or emitted indices.
        pts_.reserve(points.size() + 3);
        const size_t verticesWithSuper = points.size() + 3;
        if (verticesWithSuper <=
            (std::numeric_limits<size_t>::max() - 8) / 2) {
            tris_.reserve(verticesWithSuper * 2 + 8);
        }
        if (diagnostics_) {
            diagnostics_->initialPointCapacity = pts_.capacity();
            diagnostics_->initialTriangleCapacity = tris_.capacity();
        }
    }

    std::vector<uint32_t> run() {
        const uint32_t n = static_cast<uint32_t>(pts_.size());
        if (n < 3) return {};

        using Clock = std::chrono::steady_clock;
        const auto elapsedMs = [](const Clock::time_point& start) {
            return std::chrono::duration<double, std::milli>(Clock::now() - start)
                .count();
        };
        if (diagnostics_) {
            auto start = Clock::now();
            buildSuperTriangle(n);
            diagnostics_->superTriangleMs += elapsedMs(start);
            start = Clock::now();
            for (uint32_t i = 0; i < n; ++i) insertPoint(i);
            diagnostics_->pointInsertMs += elapsedMs(start);
        } else {
            buildSuperTriangle(n);
            for (uint32_t i = 0; i < n; ++i) insertPoint(i);
        }
        buildEdgeCounts();
        if (diagnostics_) {
            auto start = Clock::now();
            for (const auto& e : constraints_)
                insertConstraint(e.first, e.second);
            diagnostics_->constraintInsertMs += elapsedMs(start);
            start = Clock::now();
            auto result = extractInside(n);
            diagnostics_->extractInsideMs += elapsedMs(start);
            return result;
        }
        for (const auto& e : constraints_)
            insertConstraint(e.first, e.second);
        return extractInside(n);
    }

private:
    std::vector<Vec2> pts_;
    const std::vector<ConstrainedDelaunay::Edge>& constraints_;
    std::vector<Tri> tris_;
    ConstrainedDelaunayDiagnostics* diagnostics_ = nullptr;
    std::unordered_map<uint64_t, uint32_t> edgeCounts_;
    bool trackEdges_ = false;
    std::vector<Tri> pointBadTrianglesScratch_;
    std::vector<uint64_t> pointDirectedEdgesScratch_;
    std::vector<uint32_t> constraintCrossedIndicesScratch_;
    std::vector<std::array<uint32_t, 2>> constraintCrossedEdgesScratch_;
    std::unordered_set<uint64_t> constraintDirectedEdgesScratch_;
    std::unordered_map<uint32_t, uint32_t> constraintNextScratch_;
    std::unordered_set<uint32_t> constraintCrossedSetScratch_;
    std::vector<uint32_t> constraintCycleScratch_;

    void addTriEdges(const Tri& t) {
        ++edgeCounts_[edgeKey(t.v[0], t.v[1])];
        ++edgeCounts_[edgeKey(t.v[1], t.v[2])];
        ++edgeCounts_[edgeKey(t.v[2], t.v[0])];
    }

    void removeTriEdges(const Tri& t) {
        for (int k = 0; k < 3; ++k) {
            const uint64_t key = edgeKey(t.v[k], t.v[(k + 1) % 3]);
            const auto it = edgeCounts_.find(key);
            assert(it != edgeCounts_.end() && it->second > 0);
            if (it == edgeCounts_.end()) continue;
            if (it->second <= 1) edgeCounts_.erase(it);
            else --it->second;
        }
    }

    void buildEdgeCounts() {
        edgeCounts_.clear();
        edgeCounts_.reserve(tris_.size() * 2);
        for (const Tri& t : tris_) addTriEdges(t);
        trackEdges_ = true;
    }

    void recordPeakTriangles() {
        if (diagnostics_) {
            diagnostics_->peakTriangles =
                std::max(diagnostics_->peakTriangles, tris_.size());
        }
    }

    // 追加 CCW 三角形。
    void addTriCcw(uint32_t a, uint32_t b, uint32_t c) {
        if (orient2d(pts_[a], pts_[b], pts_[c]) < 0.0) std::swap(b, c);
        if (diagnostics_ && tris_.size() == tris_.capacity()) {
            ++diagnostics_->triangleCapacityGrowths;
        }
        tris_.push_back(Tri{{a, b, c}});
        if (trackEdges_) addTriEdges(tris_.back());
    }

    void buildSuperTriangle(uint32_t n) {
        double minx = pts_[0].x, maxx = pts_[0].x;
        double miny = pts_[0].y, maxy = pts_[0].y;
        for (uint32_t i = 1; i < n; ++i) {
            minx = std::min(minx, pts_[i].x);
            maxx = std::max(maxx, pts_[i].x);
            miny = std::min(miny, pts_[i].y);
            maxy = std::max(maxy, pts_[i].y);
        }
        const double dx = maxx - minx, dy = maxy - miny;
        const double dmax = std::max(dx, dy) > 0 ? std::max(dx, dy) : 1.0;
        const double midx = 0.5 * (minx + maxx), midy = 0.5 * (miny + maxy);
        // 足够大的包围三角形(CCW)。
        const auto appendPoint = [&](const Vec2& point) {
            if (diagnostics_ && pts_.size() == pts_.capacity()) {
                ++diagnostics_->pointCapacityGrowths;
            }
            pts_.push_back(point);
        };
        appendPoint(Vec2(midx - 20.0 * dmax, midy - dmax));       // n
        appendPoint(Vec2(midx + 20.0 * dmax, midy - dmax));       // n+1
        appendPoint(Vec2(midx, midy + 20.0 * dmax));              // n+2
        addTriCcw(n, n + 1, n + 2);
        recordPeakTriangles();
    }

    // Bowyer-Watson:插入点 i,重构受影响 cavity。
    void insertPoint(uint32_t i) {
        const Vec2& p = pts_[i];
        auto& badTriangles = pointBadTrianglesScratch_;
        auto& directed = pointDirectedEdgesScratch_;
        badTriangles.clear();
        directed.clear();
        // 稳定原地压缩 survivor：保持旧 kept vector 的遍历/输出顺序，但避免
        // 每个输入点都分配并复制一整份当前三角形表。
        size_t keptCount = 0;

        for (size_t read = 0; read < tris_.size(); ++read) {
            // No push_back occurs during this scan, so the reference remains
            // valid. Most triangles survive in place; avoid copying every
            // tested Tri and copy only when stable compaction has an actual
            // gap. Because keptCount <= read, writing an earlier slot cannot
            // modify the referenced read slot or change predicate order.
            const Tri& t = tris_[read];
            if (diagnostics_) ++diagnostics_->pointTriangleTests;
            const bool inside = inCircleUnsigned(
                pts_[t.v[0]], pts_[t.v[1]], pts_[t.v[2]], p);
            if (inside) {
                if (diagnostics_) ++diagnostics_->pointBadTriangles;
                // Retain the triangle once. The two following passes expand
                // its edges in the same 0->1, 1->2, 2->0 order as the former
                // three edge pushes, halving scratch writes without changing
                // hash insertion/look-up order or emitted triangles.
                badTriangles.push_back(t);
            } else {
                if (keptCount != read) {
                    tris_[keptCount] = t;
                }
                ++keptCount;
            }
        }
        if (badTriangles.empty()) return;  // p 不在任何外接圆内(理论上不该发生)

        // 有向边集合;边界 = reverse 不存在的有向边。Membership 不消费
        // hash iteration order，且最终发射仍由 badTriangles 的稳定顺序驱动；
        // 因此复用连续 scratch 并排序查询副本，避免每个输入点为 cavity
        // 的 3*bad 边构造/销毁独立 unordered_set 节点。
        directed.reserve(badTriangles.size() * 3);
        for (const Tri& t : badTriangles) {
            for (int k = 0; k < 3; ++k) {
                directed.push_back(
                    (static_cast<uint64_t>(t.v[k]) << 32) |
                    t.v[(k + 1) % 3]);
            }
        }
        std::sort(directed.begin(), directed.end());

        tris_.resize(keptCount);
        for (const Tri& t : badTriangles) {
            for (int k = 0; k < 3; ++k) {
                const uint32_t a = t.v[k];
                const uint32_t b = t.v[(k + 1) % 3];
                const uint64_t rev =
                    (static_cast<uint64_t>(b) << 32) | a;
                if (!std::binary_search(directed.begin(), directed.end(), rev)) {
                    // Boundary directed edge a->b; preserve the old edge
                    // expansion order and append (a,b,p) unchanged.
                    if (diagnostics_ && tris_.size() == tris_.capacity()) {
                        ++diagnostics_->triangleCapacityGrowths;
                    }
                    tris_.push_back(Tri{{a, b, i}});
                }
            }
        }
        recordPeakTriangles();
    }

    bool triangleHasEdge(const Tri& t, uint32_t a, uint32_t b) const {
        int found = 0;
        for (int k = 0; k < 3; ++k)
            if (t.v[k] == a || t.v[k] == b) ++found;
        return found == 2;
    }

    bool edgeExists(uint32_t a, uint32_t b) const {
        if (diagnostics_) ++diagnostics_->constraintEdgeLookups;
        return edgeCounts_.find(edgeKey(a, b)) != edgeCounts_.end();
    }

    // Anglada:插入约束边 (a,b)。
    void insertConstraint(uint32_t a, uint32_t b) {
        if (a == b) return;
        if (edgeExists(a, b)) {
            if (diagnostics_) ++diagnostics_->constraintsAlreadyPresent;
            return;
        }
        const Vec2& pa = pts_[a];
        const Vec2& pb = pts_[b];

        // 找被 ab 真穿越的三角形(任一边与 ab 真相交)。
        auto& crossedIdx = constraintCrossedIndicesScratch_;
        auto& crossedEdges = constraintCrossedEdgesScratch_;
        crossedIdx.clear();
        crossedEdges.clear();
        for (uint32_t ti = 0; ti < tris_.size(); ++ti) {
            if (diagnostics_) ++diagnostics_->constraintCrossTriangleTests;
            const Tri& t = tris_[ti];
            bool crossed = false;
            for (int k = 0; k < 3; ++k) {
                const uint32_t u = t.v[k], v = t.v[(k + 1) % 3];
                if (u == a || u == b || v == a || v == b) continue;  // 共端点跳过
                if (segmentsProperlyCross(pa, pb, pts_[u], pts_[v])) {
                    crossed = true;
                    break;
                }
            }
            if (crossed) {
                crossedIdx.push_back(ti);
                crossedEdges.push_back({t.v[0], t.v[1]});
                crossedEdges.push_back({t.v[1], t.v[2]});
                crossedEdges.push_back({t.v[2], t.v[0]});
            }
        }
        if (crossedIdx.empty()) return;  // 防御:无穿越却不存在边

        // cavity 有向边界。
        auto& directed = constraintDirectedEdgesScratch_;
        directed.clear();
        directed.reserve(crossedEdges.size() * 2);
        for (const auto& e : crossedEdges)
            directed.insert((static_cast<uint64_t>(e[0]) << 32) | e[1]);
        auto& nextMap = constraintNextScratch_;
        nextMap.clear();
        nextMap.reserve(crossedEdges.size());
        for (const auto& e : crossedEdges) {
            const uint64_t rev = (static_cast<uint64_t>(e[1]) << 32) | e[0];
            if (directed.find(rev) == directed.end()) nextMap[e[0]] = e[1];
        }

        // 移除被穿越三角形。
        auto& crossedSet = constraintCrossedSetScratch_;
        crossedSet.clear();
        crossedSet.reserve(crossedIdx.size() * 2);
        crossedSet.insert(crossedIdx.begin(), crossedIdx.end());
        size_t keptCount = 0;
        for (uint32_t ti = 0; ti < tris_.size(); ++ti) {
            const Tri t = tris_[ti];
            if (crossedSet.find(ti) == crossedSet.end()) {
                if (keptCount != ti) tris_[keptCount] = t;
                ++keptCount;
            } else {
                removeTriEdges(t);
            }
        }
        tris_.resize(keptCount);

        // 沿边界 CCW 走一圈(a→…→b→…→a),按 b 拆成两条 a→b 链。
        auto& cycle = constraintCycleScratch_;
        cycle.clear();
        cycle.reserve(nextMap.size() + 1);
        cycle.push_back(a);
        uint32_t cur = a;
        bool ok = true;
        for (size_t guard = 0; guard < nextMap.size() + 2; ++guard) {
            auto it = nextMap.find(cur);
            if (it == nextMap.end()) { ok = false; break; }
            cur = it->second;
            if (cur == a) break;
            cycle.push_back(cur);
        }
        if (!ok) return;

        size_t idxB = cycle.size();
        for (size_t k = 0; k < cycle.size(); ++k)
            if (cycle[k] == b) { idxB = k; break; }
        if (idxB == cycle.size()) return;  // 防御

        std::vector<uint32_t> sideA(cycle.begin(), cycle.begin() + idxB + 1);
        std::vector<uint32_t> sideB(cycle.begin() + idxB, cycle.end());
        sideB.push_back(a);
        // sideB 是 b→…→a,反转成 a→…→b。
        std::reverse(sideB.begin(), sideB.end());

        triangulatePseudo(sideA);
        triangulatePseudo(sideB);
        if (diagnostics_) ++diagnostics_->constraintsInserted;
        recordPeakTriangles();
    }

    // 递归三角化伪多边形:poly[0]=a … poly.back()=b,base 边 (a,b)。
    void triangulatePseudo(const std::vector<uint32_t>& poly) {
        const size_t m = poly.size();
        if (m < 3) return;
        if (m == 3) {
            addTriCcw(poly[0], poly[1], poly[2]);
            return;
        }
        const uint32_t a = poly.front();
        const uint32_t b = poly.back();
        size_t ci = 1;
        for (size_t i = 2; i + 1 < m; ++i) {
            if (inCircleUnsigned(pts_[a], pts_[b], pts_[poly[ci]], pts_[poly[i]])) {
                ci = i;
            }
        }
        addTriCcw(a, poly[ci], b);
        std::vector<uint32_t> left(poly.begin(), poly.begin() + ci + 1);
        std::vector<uint32_t> right(poly.begin() + ci, poly.end());
        triangulatePseudo(left);
        triangulatePseudo(right);
    }

    // 邻接 flood-fill 标记内/外(约束边为墙,奇偶翻转;超三角形种子=外)。
    std::vector<uint32_t> extractInside(uint32_t n) {
        std::unordered_set<uint64_t> constraintSet;
        for (const auto& e : constraints_)
            constraintSet.insert(edgeKey(e.first, e.second));

        // 邻接:无向边 → 共享它的三角形索引。
        std::unordered_map<uint64_t, std::array<int, 2>> edgeTris;
        auto addEdgeTri = [&](uint32_t u, uint32_t v, int ti) {
            const uint64_t k = edgeKey(u, v);
            auto it = edgeTris.find(k);
            if (it == edgeTris.end()) {
                edgeTris[k] = {ti, -1};
            } else if (it->second[1] == -1) {
                it->second[1] = ti;
            }
        };
        for (uint32_t ti = 0; ti < tris_.size(); ++ti) {
            const Tri& t = tris_[ti];
            addEdgeTri(t.v[0], t.v[1], ti);
            addEdgeTri(t.v[1], t.v[2], ti);
            addEdgeTri(t.v[2], t.v[0], ti);
        }

        std::vector<int> label(tris_.size(), -1);  // -1 未知,0 外,1 内
        std::deque<uint32_t> q;
        for (uint32_t ti = 0; ti < tris_.size(); ++ti) {
            const Tri& t = tris_[ti];
            if (t.v[0] >= n || t.v[1] >= n || t.v[2] >= n) {
                if (label[ti] == -1) { label[ti] = 0; q.push_back(ti); }
            }
        }
        // 若无超三角形残留(不该),回退:任取一个含超顶点的?已处理。
        while (!q.empty()) {
            const uint32_t ti = q.front();
            q.pop_front();
            const Tri& t = tris_[ti];
            for (int k = 0; k < 3; ++k) {
                const uint32_t u = t.v[k], v = t.v[(k + 1) % 3];
                const uint64_t ek = edgeKey(u, v);
                const auto& pair = edgeTris[ek];
                const int other = (pair[0] == static_cast<int>(ti)) ? pair[1] : pair[0];
                if (other < 0 || label[other] != -1) continue;
                const bool wall = constraintSet.find(ek) != constraintSet.end();
                label[other] = wall ? (1 - label[ti]) : label[ti];
                q.push_back(other);
            }
        }

        std::vector<uint32_t> out;
        for (uint32_t ti = 0; ti < tris_.size(); ++ti) {
            const Tri& t = tris_[ti];
            if (label[ti] != 1) continue;
            if (t.v[0] >= n || t.v[1] >= n || t.v[2] >= n) continue;
            // 输出 CCW。
            uint32_t x = t.v[0], y = t.v[1], z = t.v[2];
            if (orient2d(pts_[x], pts_[y], pts_[z]) < 0.0) std::swap(y, z);
            out.push_back(x);
            out.push_back(y);
            out.push_back(z);
        }
        return out;
    }
};

} // namespace

std::vector<uint32_t> ConstrainedDelaunay::triangulate(
    const std::vector<glm::dvec2>& points,
    const std::vector<Edge>& constraintEdges,
    ConstrainedDelaunayDiagnostics* diagnostics) {
    Cdt cdt(points, constraintEdges, diagnostics);
    return cdt.run();
}

} // namespace earth_engine
