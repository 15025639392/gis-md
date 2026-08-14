#include "VectorTileTree.h"

#include <algorithm>
#include <cmath>
#include <functional>

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

/// 理想瓦缺失时向上找已加载祖先顶替的最大级差(对拍 maplibre
/// TileManager.maxUnderzooming=10;更粗就糊到没有信息量了)。
constexpr int kMaxAncestorStandinLevels = 10;

/// 拉远时向下找已加载后代顶替的最大级差。3 级 = 4^3=64 倍瓦数上限,
/// 覆盖"z14 看完直接跳 z11"的真实跳档;更深的后代拼贴 draw 数不划算。
constexpr int kMaxDescendantStandinLevels = 3;

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

    // ---- R*:置换式细化选择(2026-08-15,V24 根修)----
    //
    // 对拍本仓库地形 Tileset 的 replacement refinement 与 maplibre
    // _updateRetainedTiles,但取"全有全无"语义消灭重叠:节点的子树在视口
    // 内凑不齐完整覆盖时,回退画节点自身(已加载才行);凑得齐才画子树。
    // 我们是世界空间渲染、无逐瓦 stencil 裁剪 —— 祖先与子瓦同框不是
    // "多画一点"而是要素重影,所以 renderTiles 必须是**精确覆盖**:
    // 视口内无重叠,且只要沿途有任何存货就无空洞。
    //
    // 成本上界:理想层之上的金字塔 ≈ 1.33×理想瓦数次哈希;理想层之下
    // 只在"квad 不完整且更深层确有存货"时才下探(loadedPerZ_ 早退),
    // 最坏(跳档拉远的过渡帧)~5k 次哈希,微秒级,收敛后归零。

    // 每层视口瓦范围(理想层上下各外延 stand-in 级差)
    const int zRoot = std::max(options_.minZoom, zoom - kMaxAncestorStandinLevels);
    const int zDeep = std::min(zoom + kMaxDescendantStandinLevels,
                               options_.maxZoom);
    std::vector<std::vector<Range>> rangesByLevel(
        static_cast<size_t>(zDeep - zRoot + 1));
    for (int z = zRoot; z <= zDeep; ++z) {
        for (const Rectangle& span : spans) {
            Range r{};
            scheme_->tileRange(span, z, r.minX, r.minY, r.maxX, r.maxY);
            rangesByLevel[static_cast<size_t>(z - zRoot)].push_back(r);
        }
    }
    auto inView = [&](int z, int x, int y) {
        if (z < zRoot || z > zDeep) return false;
        for (const Range& r : rangesByLevel[static_cast<size_t>(z - zRoot)]) {
            if (x >= r.minX && x <= r.maxX && y >= r.minY && y <= r.maxY) {
                return true;
            }
        }
        return false;
    };
    auto anyLoadedBelow = [&](int z) {
        for (int q = z + 1; q <= zDeep; ++q) {
            if (loadedPerZ_[static_cast<size_t>(q)] > 0) return true;
        }
        return false;
    };

    struct PendingRequest {
        TileKey key;
        long long distanceSq;
    };
    std::vector<PendingRequest> requests;
    std::vector<TileKey> emitted;

    // 返回 true = t 的视口内区域已被 emitted 完整覆盖
    std::function<bool(const TileKey&)> resolve =
        [&](const TileKey& t) -> bool {
        const bool isLoaded = loaded_.count(t) != 0;
        // 沿途所有已加载节点都 touch:未上屏的存货(等 quad 凑齐的细瓦、
        // 备用的粗瓦)同样是 retain 的一部分,不 touch 会被 LRU 抽走,
        // 抖回来就得重拉 —— 那正是缺陷③。
        if (isLoaded) touch(t);
        if (t.z == zoom && !isLoaded && !pending_.count(t) &&
            !failed_.count(t)) {
            // 理想层缺瓦无论回退成不成都要请求:后代顶替只是过渡态,
            // 不请求理想瓦就永远收敛不到目标层(4^k 倍 draw 白付)。
            const long long dx = t.x - centerKey.x;
            const long long dy = t.y - centerKey.y;
            requests.push_back({t, dx * dx + dy * dy});
        }
        if (t.z >= zoom) {
            if (isLoaded) {
                emitted.push_back(t);
                return true;
            }
            if (t.z >= zDeep || !anyLoadedBelow(t.z)) return false;
        }
        // 子树探查(全有全无)
        const size_t mark = emitted.size();
        bool all = true;
        int considered = 0;
        for (int cy = 0; cy <= 1; ++cy) {
            for (int cx = 0; cx <= 1; ++cx) {
                TileKey c{schemeId_, t.z + 1, t.x * 2 + cx, t.y * 2 + cy};
                if (!inView(c.z, c.x, c.y)) continue;
                ++considered;
                if (!resolve(c)) all = false;
            }
        }
        if (considered > 0 && all) return true;
        emitted.resize(mark);  // 凑不齐:整支回滚,决不留半个 quad
        if (t.z < zoom && isLoaded) {
            emitted.push_back(t);
            return true;
        }
        return false;
    };

    // 根:锚在 zRoot 层(跨反经线两段在粗层可能落进同一根,set 去重)
    std::unordered_set<TileKey> roots;
    for (const Range& r : rangesByLevel[0]) {
        for (int y = r.minY; y <= r.maxY; ++y) {
            for (int x = r.minX; x <= r.maxX; ++x) {
                roots.insert(TileKey{schemeId_, zRoot, x, y});
            }
        }
    }
    for (const TileKey& root : roots) {
        resolve(root);
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
    result.renderTiles = std::move(emitted);
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
    provideShared(key, std::make_shared<const MvtTile>(std::move(tile)));
}

void VectorTileTree::provideShared(const TileKey& key,
                                   std::shared_ptr<const MvtTile> tile) {
    if (!tile) {
        markFailed(key);
        return;
    }
    pending_.erase(key);
    failed_.erase(key);
    auto [it, inserted] = loaded_.try_emplace(key);
    if (inserted && key.z >= 0 &&
        key.z < static_cast<int>(loadedPerZ_.size())) {
        ++loadedPerZ_[static_cast<size_t>(key.z)];
    }
    it->second.tile = std::move(tile);
    it->second.lastUsedFrame = frame_;
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
        const TileKey& victim = candidates[i].key;
        if (victim.z >= 0 &&
            victim.z < static_cast<int>(loadedPerZ_.size())) {
            --loadedPerZ_[static_cast<size_t>(victim.z)];
        }
        loaded_.erase(victim);
    }
}

} // namespace earth_engine
