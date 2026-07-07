#include "ConstrainedDelaunay.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
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
        const std::vector<ConstrainedDelaunay::Edge>& constraints)
        : pts_(points), constraints_(constraints) {}

    std::vector<uint32_t> run() {
        const uint32_t n = static_cast<uint32_t>(pts_.size());
        if (n < 3) return {};

        buildSuperTriangle(n);
        for (uint32_t i = 0; i < n; ++i) insertPoint(i);
        for (const auto& e : constraints_) insertConstraint(e.first, e.second);
        return extractInside(n);
    }

private:
    std::vector<Vec2> pts_;
    const std::vector<ConstrainedDelaunay::Edge>& constraints_;
    std::vector<Tri> tris_;

    // 追加 CCW 三角形。
    void addTriCcw(uint32_t a, uint32_t b, uint32_t c) {
        if (orient2d(pts_[a], pts_[b], pts_[c]) < 0.0) std::swap(b, c);
        tris_.push_back(Tri{{a, b, c}});
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
        pts_.push_back(Vec2(midx - 20.0 * dmax, midy - dmax));       // n
        pts_.push_back(Vec2(midx + 20.0 * dmax, midy - dmax));       // n+1
        pts_.push_back(Vec2(midx, midy + 20.0 * dmax));              // n+2
        addTriCcw(n, n + 1, n + 2);
    }

    // Bowyer-Watson:插入点 i,重构受影响 cavity。
    void insertPoint(uint32_t i) {
        const Vec2& p = pts_[i];
        // 有向边计数,提取 cavity 边界(reverse 不在集合中的有向边)。
        std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> boundary;
        std::vector<Tri> kept;
        kept.reserve(tris_.size());
        std::vector<std::array<uint32_t, 2>> badEdges;

        for (const Tri& t : tris_) {
            if (inCircleUnsigned(pts_[t.v[0]], pts_[t.v[1]], pts_[t.v[2]], p)) {
                badEdges.push_back({t.v[0], t.v[1]});
                badEdges.push_back({t.v[1], t.v[2]});
                badEdges.push_back({t.v[2], t.v[0]});
            } else {
                kept.push_back(t);
            }
        }
        if (badEdges.empty()) return;  // p 不在任何外接圆内(理论上不该发生)

        // 有向边集合;边界 = reverse 不存在的有向边。
        std::unordered_set<uint64_t> directed;
        directed.reserve(badEdges.size() * 2);
        for (const auto& e : badEdges)
            directed.insert((static_cast<uint64_t>(e[0]) << 32) | e[1]);

        tris_ = std::move(kept);
        for (const auto& e : badEdges) {
            const uint64_t rev = (static_cast<uint64_t>(e[1]) << 32) | e[0];
            if (directed.find(rev) == directed.end()) {
                // 边界有向边 (e0->e1);新三角形 (e0,e1,p) 保持 CCW。
                tris_.push_back(Tri{{e[0], e[1], i}});
            }
        }
    }

    bool triangleHasEdge(const Tri& t, uint32_t a, uint32_t b) const {
        int found = 0;
        for (int k = 0; k < 3; ++k)
            if (t.v[k] == a || t.v[k] == b) ++found;
        return found == 2;
    }

    bool edgeExists(uint32_t a, uint32_t b) const {
        for (const Tri& t : tris_)
            if (triangleHasEdge(t, a, b)) return true;
        return false;
    }

    // Anglada:插入约束边 (a,b)。
    void insertConstraint(uint32_t a, uint32_t b) {
        if (a == b) return;
        if (edgeExists(a, b)) return;
        const Vec2& pa = pts_[a];
        const Vec2& pb = pts_[b];

        // 找被 ab 真穿越的三角形(任一边与 ab 真相交)。
        std::vector<uint32_t> crossedIdx;
        std::vector<std::array<uint32_t, 2>> crossedEdges;
        for (uint32_t ti = 0; ti < tris_.size(); ++ti) {
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
        std::unordered_set<uint64_t> directed;
        for (const auto& e : crossedEdges)
            directed.insert((static_cast<uint64_t>(e[0]) << 32) | e[1]);
        std::unordered_map<uint32_t, uint32_t> nextMap;
        for (const auto& e : crossedEdges) {
            const uint64_t rev = (static_cast<uint64_t>(e[1]) << 32) | e[0];
            if (directed.find(rev) == directed.end()) nextMap[e[0]] = e[1];
        }

        // 移除被穿越三角形。
        std::unordered_set<uint32_t> crossedSet(crossedIdx.begin(),
                                                crossedIdx.end());
        std::vector<Tri> kept;
        kept.reserve(tris_.size());
        for (uint32_t ti = 0; ti < tris_.size(); ++ti)
            if (crossedSet.find(ti) == crossedSet.end()) kept.push_back(tris_[ti]);
        tris_ = std::move(kept);

        // 沿边界 CCW 走一圈(a→…→b→…→a),按 b 拆成两条 a→b 链。
        std::vector<uint32_t> cycle;
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
    const std::vector<Edge>& constraintEdges) {
    Cdt cdt(points, constraintEdges);
    return cdt.run();
}

} // namespace earth_engine
