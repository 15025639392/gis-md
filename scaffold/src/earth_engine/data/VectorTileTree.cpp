#include "VectorTileTree.h"

#include <algorithm>
#include <cmath>

namespace earth_engine {

namespace {

/// 视口矩形拆成不跨反经线的段(west > east 时拆两段)。
std::vector<Rectangle> splitAntimeridian(const Rectangle& rect) {
    constexpr double kPi = 3.14159265358979323846;
    if (rect.west() <= rect.east()) {
        return {rect};
    }
    return {
        Rectangle(rect.west(), rect.south(), kPi, rect.north()),
        Rectangle(-kPi, rect.south(), rect.east(), rect.north()),
    };
}

} // namespace

VectorTileTree::VectorTileTree() : VectorTileTree(Options{}) {}

VectorTileTree::VectorTileTree(Options options)
    : options_(options),
      scheme_(TileScheme::createXYZWebMercator()),
      schemeId_(scheme_->id()) {}

int VectorTileTree::zoomForCameraHeight(double cameraHeightMeters,
                                        const Options& options) {
    double zoom = std::log2(4.0e7 / std::max(1.0, cameraHeightMeters)) +
                  options.zoomBias;
    int z = static_cast<int>(std::floor(std::max(0.0, zoom)));
    return std::clamp(z, options.minZoom, options.maxZoom);
}

VectorTileTree::UpdateResult VectorTileTree::update(
    const Rectangle& viewRect, double cameraHeightMeters) {
    ++frame_;
    UpdateResult result;

    std::vector<Rectangle> spans = splitAntimeridian(viewRect);

    // 目标 zoom:视高定档;desired 超闸则整体降 zoom 重枚举
    // (掠视地平线矩形在高 zoom 会爆炸,不能只截断——截断会
    // 让留下的瓦片集偏向枚举顺序而不是视口代表性)。
    int zoom = zoomForCameraHeight(cameraHeightMeters, options_);
    struct Range {
        int minX, minY, maxX, maxY;
    };
    std::vector<Range> ranges;
    long long desiredCount = 0;
    for (; zoom >= options_.minZoom; --zoom) {
        ranges.clear();
        desiredCount = 0;
        for (const Rectangle& span : spans) {
            Range r{};
            scheme_->tileRange(span, zoom, r.minX, r.minY, r.maxX, r.maxY);
            ranges.push_back(r);
            desiredCount += static_cast<long long>(r.maxX - r.minX + 1) *
                            (r.maxY - r.minY + 1);
        }
        if (desiredCount <= options_.maxTilesPerView) {
            break;
        }
    }
    zoom = std::max(zoom, options_.minZoom);

    // 视口中心(瓦片单位,用第一段的中心;跨反经线时求心不重要,
    // 只影响请求排序)
    double centerLng = (spans[0].west() + spans[0].east()) * 0.5;
    double centerLat = (viewRect.south() + viewRect.north()) * 0.5;
    TileKey centerKey = scheme_->positionToTile(centerLng, centerLat, zoom);

    std::unordered_set<TileKey> renderSet;
    struct PendingRequest {
        TileKey key;
        long long distanceSq;
    };
    std::vector<PendingRequest> requests;

    for (const Range& r : ranges) {
        for (int y = r.minY; y <= r.maxY; ++y) {
            for (int x = r.minX; x <= r.maxX; ++x) {
                TileKey key{schemeId_, zoom, x, y};
                if (loaded_.count(key)) {
                    if (renderSet.insert(key).second) {
                        touch(key);
                    }
                    continue;
                }
                // 祖先回退:向上找最近的已加载粗瓦片顶住
                TileKey ancestor = key;
                while (ancestor.z > options_.minZoom) {
                    ancestor = ancestor.parent();
                    if (loaded_.count(ancestor)) {
                        if (renderSet.insert(ancestor).second) {
                            touch(ancestor);
                        }
                        break;
                    }
                }
                if (!pending_.count(key) && !failed_.count(key)) {
                    long long dx = x - centerKey.x;
                    long long dy = y - centerKey.y;
                    requests.push_back({key, dx * dx + dy * dy});
                }
            }
        }
    }

    // 中心优先请求
    std::sort(requests.begin(), requests.end(),
              [](const PendingRequest& a, const PendingRequest& b) {
                  if (a.distanceSq != b.distanceSq) {
                      return a.distanceSq < b.distanceSq;
                  }
                  if (a.key.y != b.key.y) {
                      return a.key.y < b.key.y;
                  }
                  return a.key.x < b.key.x;
              });
    result.requestTiles.reserve(requests.size());
    for (const PendingRequest& r : requests) {
        pending_.insert(r.key);
        result.requestTiles.push_back(r.key);
    }

    // 渲染列表先粗后细(粗瓦片先画,细瓦片在其上;世界空间渲染下
    // 顺序只影响同深度 blend 的观感,保持确定性即可)
    result.renderTiles.assign(renderSet.begin(), renderSet.end());
    std::sort(result.renderTiles.begin(), result.renderTiles.end(),
              [](const TileKey& a, const TileKey& b) {
                  if (a.z != b.z) {
                      return a.z < b.z;
                  }
                  if (a.y != b.y) {
                      return a.y < b.y;
                  }
                  return a.x < b.x;
              });

    evictOverBudget();
    return result;
}

void VectorTileTree::provide(const TileKey& key, MvtTile tile) {
    pending_.erase(key);
    failed_.erase(key);
    CachedTile& entry = loaded_[key];
    entry.tile = std::make_shared<const MvtTile>(std::move(tile));
    entry.lastUsedFrame = frame_;
}

void VectorTileTree::markFailed(const TileKey& key) {
    pending_.erase(key);
    failed_.insert(key);
}

void VectorTileTree::clearFailed() { failed_.clear(); }

std::shared_ptr<const MvtTile> VectorTileTree::loadedTileShared(
    const TileKey& key) const {
    auto it = loaded_.find(key);
    return it == loaded_.end() ? nullptr : it->second.tile;
}

const MvtTile* VectorTileTree::loadedTile(const TileKey& key) const {
    auto it = loaded_.find(key);
    return it == loaded_.end() ? nullptr : it->second.tile.get();
}

void VectorTileTree::touch(const TileKey& key) {
    auto it = loaded_.find(key);
    if (it != loaded_.end()) {
        it->second.lastUsedFrame = frame_;
    }
}

void VectorTileTree::evictOverBudget() {
    if (loaded_.size() <= options_.maxCachedTiles) {
        return;
    }
    // 收集本帧未使用的,按最久未用淘汰;本帧在渲染的瓦片不淘汰
    // (预算若小于渲染集则容忍超预算,避免渲染中瓦片被抽走)
    struct Candidate {
        TileKey key;
        uint64_t lastUsedFrame;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(loaded_.size());
    for (const auto& [key, entry] : loaded_) {
        if (entry.lastUsedFrame < frame_) {
            candidates.push_back({key, entry.lastUsedFrame});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.lastUsedFrame < b.lastUsedFrame;
              });
    size_t excess = loaded_.size() - options_.maxCachedTiles;
    for (size_t i = 0; i < candidates.size() && excess > 0; ++i, --excess) {
        loaded_.erase(candidates[i].key);
    }
}

} // namespace earth_engine
