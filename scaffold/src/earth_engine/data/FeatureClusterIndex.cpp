#include "FeatureClusterIndex.h"

#include "FeatureStore.h"

#include <algorithm>
#include <cmath>

namespace earth_engine {

namespace {

constexpr double kPi = 3.14159265358979323846;
/// 墨卡托在极区发散,按 supercluster 同款纬度上限截断。
constexpr double kMaxMercatorLatitude = 85.05112878 * kPi / 180.0;

/// 经纬度(弧度)→ 球面墨卡托归一化 [0,1](x 东向,y 南向)。
double lngToX(double lng) { return (lng + kPi) / (2.0 * kPi); }

double latToY(double lat) {
    const double clamped =
        std::max(-kMaxMercatorLatitude, std::min(kMaxMercatorLatitude, lat));
    const double s = std::sin(clamped);
    const double y = 0.5 - 0.25 * std::log((1.0 + s) / (1.0 - s)) / kPi;
    return std::max(0.0, std::min(1.0, y));
}

Rectangle unionRect(const Rectangle& a, const Rectangle& b) {
    return Rectangle(std::min(a.west(), b.west()),
                     std::min(a.south(), b.south()),
                     std::max(a.east(), b.east()),
                     std::max(a.north(), b.north()));
}

} // namespace

void FeatureClusterIndex::build(const FeatureStore& store,
                                const FeatureClusterOptions& opts) {
    levels_.clear();
    options_ = opts;
    options_.maxZoom = std::max(options_.minZoom, options_.maxZoom);
    options_.minPoints = std::max<uint32_t>(2, options_.minPoints);

    // ---- 最细层:每个 Point 要素一个节点 ----
    std::vector<Node> finest;
    for (const auto& [id, feature] : store.features()) {
        if (feature.type != GeometryType::Point) continue;
        if (feature.rings.empty() || feature.rings[0].empty()) continue;
        const Cartographic& c = feature.rings[0][0];
        Node n;
        n.x = lngToX(c.longitude());
        n.y = latToY(c.latitude());
        n.lngSum = c.longitude();
        n.latSum = c.latitude();
        n.count = 1;
        n.featureId = id;
        n.bounds = Rectangle(c.longitude(), c.latitude(), c.longitude(),
                             c.latitude());
        finest.push_back(std::move(n));
    }
    // unordered_map 遍历序不稳定 → 按坐标定序,让 clusterId 与查询顺序
    // 在同一份数据上可复现(应用层可能缓存 id 做展开动画)。
    std::sort(finest.begin(), finest.end(), [](const Node& a, const Node& b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.featureId < b.featureId;
    });
    levels_.push_back(std::move(finest));
    if (levels_[0].empty()) {
        levels_.clear();
        return;
    }

    // ---- 逐层向粗合并 ----
    // 半径的世界尺度:zoom z 下整个世界宽 tileSizePx * 2^z 像素。
    for (int zoom = options_.maxZoom - 1; zoom >= options_.minZoom; --zoom) {
        const std::vector<Node>& prev = levels_.back();
        const double worldPx = options_.tileSizePx * std::pow(2.0, zoom);
        const double r = options_.radiusPx / std::max(1.0, worldPx);

        // 均匀网格加速邻域查询(cell 边长 = r → 只需查 3×3 邻格)。
        const int gridDim =
            std::max(1, std::min(4096, static_cast<int>(1.0 / std::max(
                                           r, 1.0 / 4096.0))));
        std::unordered_map<int64_t, std::vector<uint32_t>> grid;
        auto cellOf = [&](double v) {
            return std::max(0, std::min(gridDim - 1,
                                        static_cast<int>(v * gridDim)));
        };
        auto cellKey = [&](int cx, int cy) {
            return static_cast<int64_t>(cx) * 8192 + cy;
        };
        for (uint32_t i = 0; i < prev.size(); ++i) {
            grid[cellKey(cellOf(prev[i].x), cellOf(prev[i].y))].push_back(i);
        }

        std::vector<Node> next;
        std::vector<bool> taken(prev.size(), false);
        const double r2 = r * r;
        for (uint32_t i = 0; i < prev.size(); ++i) {
            if (taken[i]) continue;
            taken[i] = true;
            Node merged = prev[i];
            merged.children.assign(1, i);

            const int cx = cellOf(prev[i].x);
            const int cy = cellOf(prev[i].y);
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    const int nx = cx + dx;
                    const int ny = cy + dy;
                    if (nx < 0 || ny < 0 || nx >= gridDim || ny >= gridDim) {
                        continue;
                    }
                    auto it = grid.find(cellKey(nx, ny));
                    if (it == grid.end()) continue;
                    for (uint32_t j : it->second) {
                        if (taken[j]) continue;
                        const double ddx = prev[j].x - prev[i].x;
                        const double ddy = prev[j].y - prev[i].y;
                        if (ddx * ddx + ddy * ddy > r2) continue;
                        taken[j] = true;
                        merged.children.push_back(j);
                        merged.count += prev[j].count;
                        merged.lngSum += prev[j].lngSum;
                        merged.latSum += prev[j].latSum;
                        merged.bounds = unionRect(merged.bounds,
                                                  prev[j].bounds);
                    }
                }
            }

            if (merged.children.size() == 1) {
                // 邻域内独此一个:原样上浮(保持 count/featureId 语义)。
                next.push_back(std::move(merged));
                continue;
            }
            if (merged.count < options_.minPoints) {
                // 成员太少不成簇:成员各自上浮(不合并)。
                for (uint32_t child : merged.children) {
                    Node solo = prev[child];
                    solo.children.assign(1, child);
                    next.push_back(std::move(solo));
                }
                continue;
            }
            // 代表点 = 成员经纬度均值;聚合体重心按成员数加权。
            merged.featureId = kInvalidFeatureId;
            const double invCount = 1.0 / static_cast<double>(merged.count);
            merged.x = lngToX(merged.lngSum * invCount);
            merged.y = latToY(merged.latSum * invCount);
            next.push_back(std::move(merged));
        }
        levels_.push_back(std::move(next));
    }
}

size_t FeatureClusterIndex::levelIndexForZoom(double zoom) const {
    if (levels_.empty()) return 0;
    const double clamped =
        std::max(static_cast<double>(options_.minZoom),
                 std::min(static_cast<double>(options_.maxZoom), zoom));
    // levels_[0] = maxZoom,越往后越粗。
    const int offset = static_cast<int>(
        std::lround(static_cast<double>(options_.maxZoom) - clamped));
    return static_cast<size_t>(
        std::max(0, std::min(static_cast<int>(levels_.size()) - 1, offset)));
}

FeatureCluster FeatureClusterIndex::toCluster(size_t levelIndex,
                                              size_t nodeIndex) const {
    const Node& n = levels_[levelIndex][nodeIndex];
    FeatureCluster c;
    c.clusterId = makeId(levelIndex, nodeIndex);
    const double invCount = 1.0 / static_cast<double>(n.count);
    c.longitude = n.lngSum * invCount;
    c.latitude = n.latSum * invCount;
    c.count = n.count;
    c.featureId = n.count == 1 ? n.featureId : kInvalidFeatureId;
    c.bounds = n.bounds;
    return c;
}

std::vector<FeatureCluster> FeatureClusterIndex::query(const Rectangle& bbox,
                                                       double zoom) const {
    std::vector<FeatureCluster> out;
    if (levels_.empty()) return out;
    const size_t level = levelIndexForZoom(zoom);
    const auto& nodes = levels_[level];
    out.reserve(nodes.size() / 4 + 1);
    for (size_t i = 0; i < nodes.size(); ++i) {
        // 代表点在视口内即入选(簇的成员可能跨出视口,这是聚合的本意)。
        const FeatureCluster c = toCluster(level, i);
        if (c.longitude < bbox.west() || c.longitude > bbox.east() ||
            c.latitude < bbox.south() || c.latitude > bbox.north()) {
            continue;
        }
        out.push_back(c);
    }
    return out;
}

bool FeatureClusterIndex::decodeId(uint64_t id,
                                   size_t* levelIndex,
                                   size_t* nodeIndex) const {
    const size_t level = static_cast<size_t>(id >> 32);
    const size_t node = static_cast<size_t>(id & 0xFFFFFFFFull);
    if (level >= levels_.size() || node >= levels_[level].size()) return false;
    *levelIndex = level;
    *nodeIndex = node;
    return true;
}

int FeatureClusterIndex::expansionZoom(uint64_t clusterId) const {
    size_t level = 0, node = 0;
    if (!decodeId(clusterId, &level, &node)) return options_.maxZoom + 1;
    const Node* current = &levels_[level][node];
    if (current->count <= 1) return options_.maxZoom + 1;
    // 向细走,直到该簇在某层拆成多于一个节点。
    while (level > 0) {
        if (current->children.size() > 1) {
            // children 属于 level-1(更细)层 → 那一层已经散开。
            return options_.maxZoom - static_cast<int>(level - 1);
        }
        const uint32_t only = current->children.front();
        --level;
        current = &levels_[level][only];
    }
    return options_.maxZoom;
}

std::vector<FeatureId> FeatureClusterIndex::leaves(uint64_t clusterId) const {
    std::vector<FeatureId> out;
    size_t level = 0, node = 0;
    if (!decodeId(clusterId, &level, &node)) return out;

    // 显式栈递归下降到最细层(避免深层级递归)。
    std::vector<std::pair<size_t, uint32_t>> stack;
    stack.emplace_back(level, static_cast<uint32_t>(node));
    while (!stack.empty()) {
        const auto [lvl, idx] = stack.back();
        stack.pop_back();
        const Node& n = levels_[lvl][idx];
        if (lvl == 0) {
            if (n.featureId != kInvalidFeatureId) out.push_back(n.featureId);
            continue;
        }
        for (uint32_t child : n.children) {
            stack.emplace_back(lvl - 1, child);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace earth_engine
