#include "FeatureSpatialIndex.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace earth_engine {

namespace {

// R-tree 在裸平面 lng/lat AABB 上运算,不借 Rectangle 的反经线(GlobeRectangle)
// 语义 —— 后者的 computeUnion/intersects 会绕 ±2π 归一化、边界排斥,破坏 MBR
// 覆盖不变量与零面积命中。约定 bounds 非跨反经线(west<=east, south<=north)。

// 裸并集:min west/south, max east/north。
Rectangle rawUnion(const Rectangle& a, const Rectangle& b) {
    return Rectangle(std::min(a.west(), b.west()), std::min(a.south(), b.south()),
                     std::max(a.east(), b.east()), std::max(a.north(), b.north()));
}

// 裸相交(含边界):任一轴不重叠则不相交。
bool rawIntersects(const Rectangle& a, const Rectangle& b) {
    return !(a.east() < b.west() || a.west() > b.east() ||
             a.north() < b.south() || a.south() > b.north());
}

} // namespace

FeatureSpatialIndex::FeatureSpatialIndex() {
    root_ = std::make_unique<Node>();
    root_->leaf = true;
}

FeatureSpatialIndex::~FeatureSpatialIndex() = default;

// ============================================================
// 几何辅助
// ============================================================

double FeatureSpatialIndex::area(const Rectangle& r) {
    // 约定非跨反经线,直接乘。退化(点/线)面积为 0,合法。
    const double w = r.computeWidth();
    const double h = r.computeHeight();
    return w * h;
}

double FeatureSpatialIndex::enlargement(const Rectangle& current,
                                        const Rectangle& add) {
    const double before = area(current);
    const double after = area(rawUnion(current, add));
    return after - before;
}

void FeatureSpatialIndex::recomputeMbr(Node* node) {
    Rectangle mbr;
    bool first = true;
    auto absorb = [&](const Rectangle& r) {
        if (first) {
            mbr = r;
            first = false;
        } else {
            mbr = rawUnion(mbr, r);
        }
    };
    if (node->leaf) {
        for (const auto& e : node->entries) absorb(e.mbr);
    } else {
        for (const auto& c : node->children) absorb(c->mbr);
    }
    if (first) {
        node->mbr = Rectangle();  // 空节点
    } else {
        node->mbr = mbr;
    }
}

// ============================================================
// 插入
// ============================================================

void FeatureSpatialIndex::insert(FeatureId id, const Rectangle& bounds) {
    Entry e{bounds, id};
    std::unique_ptr<Node> sibling = insertRec(root_.get(), e);
    if (sibling) {
        // 根分裂,长高一层。
        auto newRoot = std::make_unique<Node>();
        newRoot->leaf = false;
        newRoot->children.push_back(std::move(root_));
        newRoot->children.push_back(std::move(sibling));
        recomputeMbr(newRoot.get());
        root_ = std::move(newRoot);
        ++rootHeight_;
    }
    ++count_;
}

FeatureSpatialIndex::Node* FeatureSpatialIndex::chooseSubtree(
    Node* node, const Rectangle& r) {
    Node* best = nullptr;
    double bestEnlarge = std::numeric_limits<double>::max();
    double bestArea = std::numeric_limits<double>::max();
    for (auto& c : node->children) {
        const double enl = enlargement(c->mbr, r);
        const double a = area(c->mbr);
        if (enl < bestEnlarge || (enl == bestEnlarge && a < bestArea)) {
            bestEnlarge = enl;
            bestArea = a;
            best = c.get();
        }
    }
    return best;
}

std::unique_ptr<FeatureSpatialIndex::Node> FeatureSpatialIndex::insertRec(
    Node* node, const Entry& e) {
    if (node->leaf) {
        node->entries.push_back(e);
        node->mbr = node->entries.size() == 1
                        ? e.mbr
                        : rawUnion(node->mbr, e.mbr);
        if (node->entries.size() > kMaxEntries) {
            return splitNode(node);
        }
        return nullptr;
    }

    Node* sub = chooseSubtree(node, e.mbr);
    std::unique_ptr<Node> split = insertRec(sub, e);
    // 子树 mbr 可能变大,先并入。
    node->mbr = rawUnion(node->mbr, e.mbr);
    if (split) {
        node->children.push_back(std::move(split));
        if (node->children.size() > kMaxEntries) {
            return splitNode(node);
        }
        recomputeMbr(node);
    }
    return nullptr;
}

// ============================================================
// 二次分裂(quadratic split)
// ============================================================

std::unique_ptr<FeatureSpatialIndex::Node> FeatureSpatialIndex::splitNode(
    Node* node) {
    auto sibling = std::make_unique<Node>();
    sibling->leaf = node->leaf;

    // 统一用一份 Rectangle 列表 + 索引做分配,再把实体搬回两个节点。
    const size_t n = node->leaf ? node->entries.size() : node->children.size();
    std::vector<Rectangle> rects(n);
    for (size_t i = 0; i < n; ++i) {
        rects[i] = node->leaf ? node->entries[i].mbr : node->children[i]->mbr;
    }

    // PickSeeds:找最浪费的一对(union 面积 - 各自面积 最大)。
    size_t seedA = 0, seedB = 1;
    double worst = -std::numeric_limits<double>::max();
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            const double d =
                area(rawUnion(rects[i], rects[j])) - area(rects[i]) - area(rects[j]);
            if (d > worst) {
                worst = d;
                seedA = i;
                seedB = j;
            }
        }
    }

    // 分配:group0 归 node(暂存),group1 归 sibling。
    std::vector<size_t> group0, group1;
    std::vector<bool> assigned(n, false);
    Rectangle mbr0 = rects[seedA];
    Rectangle mbr1 = rects[seedB];
    group0.push_back(seedA);
    group1.push_back(seedB);
    assigned[seedA] = assigned[seedB] = true;
    size_t remaining = n - 2;

    while (remaining > 0) {
        // 若某组补齐后另一组会低于 m,强制把剩余全给需要的一组。
        if (group0.size() + remaining == kMinEntries) {
            for (size_t i = 0; i < n; ++i)
                if (!assigned[i]) { group0.push_back(i); assigned[i] = true; }
            break;
        }
        if (group1.size() + remaining == kMinEntries) {
            for (size_t i = 0; i < n; ++i)
                if (!assigned[i]) { group1.push_back(i); assigned[i] = true; }
            break;
        }
        // PickNext:选偏好差最大的一项。
        size_t pick = 0;
        double bestDiff = -1.0;
        int pickGroup = 0;
        for (size_t i = 0; i < n; ++i) {
            if (assigned[i]) continue;
            const double d0 = enlargement(mbr0, rects[i]);
            const double d1 = enlargement(mbr1, rects[i]);
            const double diff = std::abs(d0 - d1);
            if (diff > bestDiff) {
                bestDiff = diff;
                pick = i;
                pickGroup = (d0 < d1) ? 0 : 1;
            }
        }
        assigned[pick] = true;
        if (pickGroup == 0) {
            group0.push_back(pick);
            mbr0 = rawUnion(mbr0, rects[pick]);
        } else {
            group1.push_back(pick);
            mbr1 = rawUnion(mbr1, rects[pick]);
        }
        --remaining;
    }

    // 搬运实体。先取出原节点内容。
    if (node->leaf) {
        std::vector<Entry> all = std::move(node->entries);
        node->entries.clear();
        for (size_t idx : group0) node->entries.push_back(all[idx]);
        for (size_t idx : group1) sibling->entries.push_back(all[idx]);
    } else {
        std::vector<std::unique_ptr<Node>> all = std::move(node->children);
        node->children.clear();
        for (size_t idx : group0) node->children.push_back(std::move(all[idx]));
        for (size_t idx : group1) sibling->children.push_back(std::move(all[idx]));
    }
    recomputeMbr(node);
    recomputeMbr(sibling.get());
    return sibling;
}

// ============================================================
// 范围查询
// ============================================================

std::vector<FeatureId> FeatureSpatialIndex::query(
    const Rectangle& queryBounds) const {
    std::vector<FeatureId> out;
    if (count_ == 0) return out;
    queryRec(root_.get(), queryBounds, out);
    return out;
}

void FeatureSpatialIndex::queryRec(const Node* node, const Rectangle& q,
                                   std::vector<FeatureId>& out) {
    if (!rawIntersects(node->mbr, q)) return;
    if (node->leaf) {
        for (const auto& e : node->entries) {
            if (rawIntersects(e.mbr, q)) out.push_back(e.id);
        }
    } else {
        for (const auto& c : node->children) {
            queryRec(c.get(), q, out);
        }
    }
}

// ============================================================
// 删除 + condense
// ============================================================

bool FeatureSpatialIndex::remove(FeatureId id, const Rectangle& bounds) {
    std::vector<Entry> reinsertEntries;
    std::vector<std::unique_ptr<Node>> reinsertNodes;
    bool found = false;
    removeRec(root_.get(), id, bounds, reinsertEntries, reinsertNodes, found);
    if (!found) return false;
    --count_;

    // 根收缩:内部根只剩一个孩子时下沉。
    while (!root_->leaf && root_->children.size() == 1) {
        root_ = std::move(root_->children.front());
        --rootHeight_;
    }
    if (root_->leaf && root_->entries.empty()) {
        root_->mbr = Rectangle();
    }

    // 重插被 condense 摘下的孤儿(先节点里的 entry,再散 entry)。
    for (auto& orphan : reinsertNodes) {
        std::vector<Entry> es;
        collectEntries(orphan.get(), es);
        for (const auto& e : es) {
            std::unique_ptr<Node> sib = insertRec(root_.get(), e);
            if (sib) {
                auto newRoot = std::make_unique<Node>();
                newRoot->leaf = false;
                newRoot->children.push_back(std::move(root_));
                newRoot->children.push_back(std::move(sib));
                recomputeMbr(newRoot.get());
                root_ = std::move(newRoot);
                ++rootHeight_;
            }
        }
    }
    for (const auto& e : reinsertEntries) {
        std::unique_ptr<Node> sib = insertRec(root_.get(), e);
        if (sib) {
            auto newRoot = std::make_unique<Node>();
            newRoot->leaf = false;
            newRoot->children.push_back(std::move(root_));
            newRoot->children.push_back(std::move(sib));
            recomputeMbr(newRoot.get());
            root_ = std::move(newRoot);
            ++rootHeight_;
        }
    }
    return true;
}

bool FeatureSpatialIndex::removeRec(
    Node* node, FeatureId id, const Rectangle& bounds,
    std::vector<Entry>& reinsertEntries,
    std::vector<std::unique_ptr<Node>>& reinsertNodes, bool& found) {
    if (!rawIntersects(node->mbr, bounds)) return false;

    if (node->leaf) {
        for (size_t i = 0; i < node->entries.size(); ++i) {
            if (node->entries[i].id == id && rawIntersects(node->entries[i].mbr, bounds)) {
                node->entries.erase(node->entries.begin() + i);
                found = true;
                recomputeMbr(node);
                return true;
            }
        }
        return false;
    }

    for (size_t i = 0; i < node->children.size(); ++i) {
        Node* child = node->children[i].get();
        if (removeRec(child, id, bounds, reinsertEntries, reinsertNodes, found)) {
            // 子树被改动。检查下溢。
            const size_t childCount =
                child->leaf ? child->entries.size() : child->children.size();
            if (childCount < kMinEntries && node->children.size() > 1) {
                // 摘下下溢子节点,其内容留待重插。
                reinsertNodes.push_back(std::move(node->children[i]));
                node->children.erase(node->children.begin() + i);
            }
            recomputeMbr(node);
            return true;
        }
    }
    return false;
}

void FeatureSpatialIndex::collectEntries(Node* node, std::vector<Entry>& out) {
    if (node->leaf) {
        for (auto& e : node->entries) out.push_back(e);
    } else {
        for (auto& c : node->children) collectEntries(c.get(), out);
    }
}

// ============================================================
// 清空
// ============================================================

void FeatureSpatialIndex::clear() {
    root_ = std::make_unique<Node>();
    root_->leaf = true;
    count_ = 0;
    rootHeight_ = 0;
}

} // namespace earth_engine
