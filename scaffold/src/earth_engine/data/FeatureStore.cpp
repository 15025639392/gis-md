#include "FeatureStore.h"

#include <algorithm>
#include <limits>

namespace earth_engine {

Rectangle FeatureStore::computeBoundsFromRings(const Feature& f) {
    double west = std::numeric_limits<double>::max();
    double east = std::numeric_limits<double>::lowest();
    double south = std::numeric_limits<double>::max();
    double north = std::numeric_limits<double>::lowest();
    bool any = false;
    for (const auto& ring : f.rings) {
        for (const auto& c : ring) {
            west = std::min(west, c.longitude());
            east = std::max(east, c.longitude());
            south = std::min(south, c.latitude());
            north = std::max(north, c.latitude());
            any = true;
        }
    }
    if (!any) return Rectangle();
    return Rectangle(west, south, east, north);
}

FeatureId FeatureStore::addFeature(Feature feature) {
    if (feature.id == kInvalidFeatureId) {
        feature.id = nextId_++;
    } else {
        // 外部指定 ID(如导入保源 ID 场景);推进分配器避免冲突。
        nextId_ = std::max(nextId_, feature.id + 1);
    }
    // bounds 权威来自 rings —— 一律重算(不依赖 Rectangle::isEmpty,其语义是
    // south>north 的反经线 sentinel,默认零矩形不算 empty 会漏算)。
    feature.bounds = computeBoundsFromRings(feature);
    const FeatureId id = feature.id;
    const Rectangle bounds = feature.bounds;
    features_[id] = std::move(feature);
    index_.insert(id, bounds);
    buckets_.assign(id, bounds);
    return id;
}

bool FeatureStore::removeFeature(FeatureId id) {
    auto it = features_.find(id);
    if (it == features_.end()) return false;
    index_.remove(id, it->second.bounds);
    buckets_.unassign(id, it->second.bounds);
    features_.erase(it);
    return true;
}

bool FeatureStore::updateFeature(const Feature& feature) {
    auto it = features_.find(feature.id);
    if (it == features_.end()) return false;

    // 先摘旧索引/桶条目(用旧 bounds),再算新 bounds、写回、插新条目。
    // 跨桶时新旧两桶都被标脏(unassign 标旧、assign 标新)。
    const Rectangle oldBounds = it->second.bounds;
    index_.remove(feature.id, oldBounds);
    buckets_.unassign(feature.id, oldBounds);

    Feature updated = feature;
    updated.bounds = computeBoundsFromRings(updated);  // 权威来自 rings
    updated.version = it->second.version + 1;
    const Rectangle newBounds = updated.bounds;
    it->second = std::move(updated);
    index_.insert(feature.id, newBounds);
    buckets_.assign(feature.id, newBounds);
    return true;
}

const Feature* FeatureStore::getFeature(FeatureId id) const {
    auto it = features_.find(id);
    return it == features_.end() ? nullptr : &it->second;
}

std::vector<FeatureId> FeatureStore::queryVisible(const Rectangle& bbox) const {
    return index_.query(bbox);
}

void FeatureStore::clear() {
    features_.clear();
    index_.clear();
    buckets_.clear();
    nextId_ = 1;
}

} // namespace earth_engine
