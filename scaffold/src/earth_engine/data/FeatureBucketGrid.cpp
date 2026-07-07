#include "FeatureBucketGrid.h"

#include <cmath>

namespace earth_engine {

namespace {

// 裸平面 AABB 相交(含边界),与 FeatureSpatialIndex 同一约定。
bool rawIntersects(const Rectangle& a, const Rectangle& b) {
    return !(a.east() < b.west() || a.west() > b.east() ||
             a.north() < b.south() || a.south() > b.north());
}

} // namespace

FeatureBucketGrid::FeatureBucketGrid(double cellSizeRadians)
    : cellSize_(cellSizeRadians) {}

BucketKey FeatureBucketGrid::packCell(int32_t cx, int32_t cy) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) |
           static_cast<uint64_t>(static_cast<uint32_t>(cy));
}

int32_t FeatureBucketGrid::cellX(BucketKey key) {
    return static_cast<int32_t>(static_cast<uint32_t>(key >> 32));
}

int32_t FeatureBucketGrid::cellY(BucketKey key) {
    return static_cast<int32_t>(static_cast<uint32_t>(key & 0xFFFFFFFFull));
}

BucketKey FeatureBucketGrid::bucketFor(const Rectangle& bounds) const {
    // 超大要素(跨 cell)走专用桶,避免中心 cell 之外的视口漏掉它。
    if (bounds.computeWidth() > cellSize_ || bounds.computeHeight() > cellSize_) {
        return kOversizedBucket;
    }
    // 裸中心(不借 Rectangle::center 的反经线语义)。
    const double cLng = 0.5 * (bounds.west() + bounds.east());
    const double cLat = 0.5 * (bounds.south() + bounds.north());
    const auto cx = static_cast<int32_t>(std::floor(cLng / cellSize_));
    const auto cy = static_cast<int32_t>(std::floor(cLat / cellSize_));
    return packCell(cx, cy);
}

BucketKey FeatureBucketGrid::assign(FeatureId id, const Rectangle& bounds) {
    const BucketKey key = bucketFor(bounds);
    members_[key].insert(id);
    dirty_.insert(key);
    return key;
}

void FeatureBucketGrid::unassign(FeatureId id, const Rectangle& bounds) {
    const BucketKey key = bucketFor(bounds);
    auto it = members_.find(key);
    if (it == members_.end()) return;
    it->second.erase(id);
    dirty_.insert(key);  // 桶内容变了 → 脏(即便变空,consumer 据此丢弃几何)
    if (it->second.empty()) {
        members_.erase(it);
    }
}

const std::unordered_set<FeatureId>* FeatureBucketGrid::featuresIn(
    BucketKey key) const {
    auto it = members_.find(key);
    return it == members_.end() ? nullptr : &it->second;
}

std::vector<BucketKey> FeatureBucketGrid::bucketsInView(
    const Rectangle& viewBbox) const {
    std::vector<BucketKey> out;
    for (const auto& kv : members_) {
        if (kv.first == kOversizedBucket) {
            out.push_back(kv.first);  // 超大桶恒纳入
            continue;
        }
        if (rawIntersects(cellRect(kv.first), viewBbox)) {
            out.push_back(kv.first);
        }
    }
    return out;
}

std::unordered_set<BucketKey> FeatureBucketGrid::consumeDirty() {
    std::unordered_set<BucketKey> taken;
    taken.swap(dirty_);
    return taken;
}

Rectangle FeatureBucketGrid::cellRect(BucketKey key) const {
    if (key == kOversizedBucket) return Rectangle();
    const int32_t cx = cellX(key);
    const int32_t cy = cellY(key);
    const double w = static_cast<double>(cx) * cellSize_;
    const double s = static_cast<double>(cy) * cellSize_;
    return Rectangle(w, s, w + cellSize_, s + cellSize_);
}

void FeatureBucketGrid::clear() {
    members_.clear();
    dirty_.clear();
}

} // namespace earth_engine
