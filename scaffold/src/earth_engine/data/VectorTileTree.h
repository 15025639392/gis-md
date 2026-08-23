#pragma once

#include "MvtDecoder.h"
#include "../core/math/Rectangle.h"
#include "../tiling/TileKey.h"
#include "../tiling/TileScheme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace earth_engine {

namespace detail {

/// 视口矩形拆成不跨反经线的段(west > east 时拆两段)。
inline std::vector<Rectangle> splitAntimeridian(const Rectangle& rect) {
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

} // namespace detail

/// 只读矢量底图的瓦片树(P4b,设计 §5)。
///
/// 与地形 Tileset 刻意分离(独立实例、独立 zoom 范围/缓存,LOD 语义
/// 不互污;TileContentLoadResult 是 glTF/地形耦合的,矢量内容不塞进去)。
/// 选择模型(R*,2026-08-15 V24 根修):视高定目标 zoom → **置换式细化**
/// —— 全有全无的祖先/后代回退,renderTiles 恒为视口内**精确覆盖**
/// (无重叠、有存货即无空洞)。语义对拍地形 Tileset 的 replacement
/// refinement 与 maplibre _updateRetainedTiles,但比 maplibre 多"无重叠"
/// 保证:我们是世界空间渲染、无逐瓦 stencil,祖先与子瓦同框即要素重影。
/// 不做地形式逐瓦片 SSE 细分。
///
/// overzoom 与 maplibre 的差异(刻意):maplibre 在瓦片裁剪空间逐瓦片
/// 渲染,超 maxZoom 必须把父瓦片几何 scale+offset+clip 切片;本引擎把
/// 要素转成世界空间几何渲染,maxZoom 瓦片的几何天然连续覆盖任意深的
/// 缩放,故 overzoom = 目标 zoom 钳制到 maxZoom,零切片。
///
/// 纯选择/缓存逻辑,不发网络请求:update 产出 requestTiles,由调用方
/// (P4c)拉取后经 provide()/markFailed() 回灌。非线程安全,调用方
/// 保证单线程访问(与 FeatureStore 同约定)。
///
/// **E3 泛化(E 方案通路):载荷类型模板化。** MVT 与高德 Nebula 共用同一
/// 套调度(视口枚举/祖先回退/后代顶替/LRU/请求去重),差异只在「解码产物
/// 长什么样」——树本身不碰载荷字段,只负责持有与生命周期,故 Payload 可以
/// 是 MvtTile、Amap 解码层列表或任何解码容器。现有 MVT 消费方用别名
/// `VectorTileTree = VectorTileTreeT<MvtTile>` 零改动接入。
template <typename Payload>
class VectorTileTreeT {
public:
    struct Options {
        int minZoom = 0;
        int maxZoom = 14;
        /// LRU 缓存瓦片数上限(设计 §5:移动端保守 200–400)。
        /// 本帧在渲染的瓦片不计入淘汰。
        size_t maxCachedTiles = 300;
        /// 单次 update 的 desired 瓦片数上限;超出自动降 zoom 重枚举
        /// (掠视地平线视口在高 zoom 会枚举出海量瓦片,必须有闸)。
        int maxTilesPerView = 64;
        /// 目标 zoom 偏置(正 = 更细)。
        double zoomBias = 0.0;
        /// 瓦片体系。空 = XYZ WebMercator(默认)。
        /// 高德用 4326 等距圆柱网格(2:1,见
        /// TileScheme::createAmapGeographic),树的选择/回退全部只经
        /// scheme 换算,不关心载荷坐标系。
        std::shared_ptr<TileScheme> scheme;
    };

    struct UpdateResult {
        /// 应渲染的已加载瓦片(含祖先回退,已去重;先粗后细)。
        std::vector<TileKey> renderTiles;
        /// 需发起请求的瓦片(视口中心优先排序;已登记 pending,
        /// 调用方必须最终以 provide()/markFailed() 回应)。
        std::vector<TileKey> requestTiles;
    };

    VectorTileTreeT() : VectorTileTreeT(Options{}) {}
    explicit VectorTileTreeT(Options options)
        : options_(options),
          scheme_(options.scheme ? std::move(options.scheme)
                                 : TileScheme::createXYZWebMercator()),
          schemeId_(scheme_->id()) {}

    /// viewRect 允许跨反经线(west > east,自动拆两段枚举)。
    /// cameraHeightMeters 为相机离地高度。
    UpdateResult update(const Rectangle& viewRect, double cameraHeightMeters) {
        ++frame_;
        UpdateResult result;

        std::vector<Rectangle> spans = detail::splitAntimeridian(viewRect);

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
        TileKey centerKey =
            scheme_->positionToTile(centerLng, centerLat, zoom);

        // ---- 逐理想瓦独立回退(2026-08-15;R* 的"全有全无"已回退,见下)----
        //
        // ⚠️ **本树只喂 POI 符号**(demo `GLESView.cpp` 里
        // `includeLayers={"poi"}`;面走 drape 页存储、线走 SDF 场,都不经
        // 这里)。这决定了重叠的代价:祖先与已加载兄弟同框 = **同一个 POI
        // 画两遍**(标签还有 crossTileID + 碰撞去重兜着),是可忍受的轻
        // 伪影;而"全有全无"为消这点重叠付出的是 **整支回滚** —— quad 里
        // 缺一块,已加载的兄弟全部作废,且因为中间层从不被请求(只请求
        // 理想层),回滚会一路级联到某个碰巧还在缓存里的很粗祖先。真机
        // 实测:z11 缺一块 → 退到 z8,42 个 POI 顶替 312 个 = 用户看到
        // "点全部消失"。**对符号,这个交换是亏的**,故只保留后代回退与
        // 存货保活,重叠回到可忍受清单。判据见 docs/northstar/vector.md B.5。
        //
        // 后代回退**仍要求完整覆盖**:它是拉远时的整块顶替(细瓦几何天然
        // 覆盖粗区域),半个 quad 顶上去会在理想瓦内部留洞,那不是"少画
        // 几个点"而是硬边界,比重叠难看。

        const int zDeep =
            std::min(zoom + detail::kMaxDescendantStandinLevels,
                     options_.maxZoom);
        std::vector<std::vector<Range>> rangesByLevel(
            static_cast<size_t>(zDeep - zoom + 1));
        for (int z = zoom; z <= zDeep; ++z) {
            for (const Rectangle& span : spans) {
                Range r{};
                scheme_->tileRange(span, z, r.minX, r.minY, r.maxX, r.maxY);
                rangesByLevel[static_cast<size_t>(z - zoom)].push_back(r);
            }
        }
        auto inView = [&](int z, int x, int y) {
            if (z < zoom || z > zDeep) return false;
            for (const Range& r :
                 rangesByLevel[static_cast<size_t>(z - zoom)]) {
                if (x >= r.minX && x <= r.maxX && y >= r.minY &&
                    y <= r.maxY) {
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

        // 后代顶替:t 的视口内区域能否被**已加载的后代完整覆盖**。
        // 沿途 touch:等着凑齐的细瓦也是 retain 的一部分,不 touch 会被
        // LRU 抽走,抖回来就得重拉(缺陷③)。
        std::function<bool(const TileKey&)> coverByDescendants =
            [&](const TileKey& t) -> bool {
            if (loaded_.count(t)) {
                touch(t);
                emitted.push_back(t);
                return true;
            }
            if (t.z >= zDeep || !anyLoadedBelow(t.z)) return false;
            const size_t mark = emitted.size();
            bool all = true;
            int considered = 0;
            for (int cy = 0; cy <= 1 && all; ++cy) {
                for (int cx = 0; cx <= 1 && all; ++cx) {
                    TileKey c{schemeId_, t.z + 1, t.x * 2 + cx, t.y * 2 + cy};
                    if (!inView(c.z, c.x, c.y)) continue;
                    ++considered;
                    if (!coverByDescendants(c)) all = false;
                }
            }
            if (considered > 0 && all) return true;
            emitted.resize(mark);  // 半个 quad 会在理想瓦内留硬边界,不要
            return false;
        };

        for (const Range& r : rangesByLevel[0]) {
            for (int y = r.minY; y <= r.maxY; ++y) {
                for (int x = r.minX; x <= r.maxX; ++x) {
                    const TileKey ideal{schemeId_, zoom, x, y};
                    if (!coverByDescendants(ideal)) {
                        // 祖先回退:向上找最近的已加载粗瓦顶住(可能与别的
                        // 理想瓦已 emit 的细瓦重叠 —— 对符号是可忍受的轻
                        // 伪影)
                        TileKey ancestor = ideal;
                        while (ancestor.z > options_.minZoom) {
                            ancestor = ancestor.parent();
                            if (loaded_.count(ancestor)) {
                                touch(ancestor);
                                emitted.push_back(ancestor);
                                break;
                            }
                        }
                    }
                    if (!loaded_.count(ideal) && !pending_.count(ideal) &&
                        !failed_.count(ideal)) {
                        // 理想瓦无论回退成不成都要请求:顶替只是过渡态
                        const long long dx = x - centerKey.x;
                        const long long dy = y - centerKey.y;
                        requests.push_back({ideal, dx * dx + dy * dy});
                    }
                }
            }
        }
        // 祖先可能被多个理想瓦共同选中 → 去重
        std::sort(emitted.begin(), emitted.end(),
                  [](const TileKey& a, const TileKey& b) {
                      if (a.z != b.z) return a.z < b.z;
                      if (a.y != b.y) return a.y < b.y;
                      return a.x < b.x;
                  });
        emitted.erase(std::unique(emitted.begin(), emitted.end()),
                      emitted.end());

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

    /// 解码完成回灌。未请求过的 key 也接受(幂等)。
    void provide(const TileKey& key, Payload tile) {
        provideShared(key, std::make_shared<const Payload>(std::move(tile)));
    }
    /// 共享回灌(获取层单一化):瓦片由获取缓存解码并共享持有,树内只存
    /// 引用 —— 同一块解码瓦在多消费方之间恰一份。空指针视为失败
    /// (等价 markFailed)。
    void provideShared(const TileKey& key,
                       std::shared_ptr<const Payload> tile) {
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
    /// 请求失败登记;失败瓦片不再重复请求,直到 clearFailed()。
    void markFailed(const TileKey& key) {
        pending_.erase(key);
        failed_.insert(key);
    }
    void clearFailed() { failed_.clear(); }

    const Payload* loadedTile(const TileKey& key) const {
        auto it = loaded_.find(key);
        return it == loaded_.end() ? nullptr : it->second.tile.get();
    }

    /// 共享持有已解码瓦片。E1:镶嵌在 worker 上跑,而树的 LRU 随时可能
    /// 淘汰该瓦片 —— worker 必须持共享所有权,不能拿裸指针。
    std::shared_ptr<const Payload> loadedTileShared(const TileKey& key) const {
        auto it = loaded_.find(key);
        return it == loaded_.end() ? nullptr : it->second.tile;
    }
    size_t loadedCount() const { return loaded_.size(); }
    size_t pendingCount() const { return pending_.size(); }
    size_t failedCount() const { return failed_.size(); }

    const TileScheme& scheme() const { return *scheme_; }
    const Options& options() const { return options_; }

    /// zoom ≈ log2(赤道周长 4e7m / 视高)(与 FeatureRenderLayer 的样式
    /// zoom 同一公式),加偏置后钳制 [minZoom, maxZoom]。
    static int zoomForCameraHeight(double cameraHeightMeters,
                                   const Options& options) {
        double zoom = std::log2(4.0e7 / std::max(1.0, cameraHeightMeters)) +
                      options.zoomBias;
        int z = static_cast<int>(std::floor(std::max(0.0, zoom)));
        return std::clamp(z, options.minZoom, options.maxZoom);
    }

private:
    struct CachedTile {
        std::shared_ptr<const Payload> tile;
        uint64_t lastUsedFrame = 0;
    };

    void touch(const TileKey& key) {
        auto it = loaded_.find(key);
        if (it != loaded_.end()) {
            it->second.lastUsedFrame = frame_;
        }
    }

    void evictOverBudget() {
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
        for (size_t i = 0; i < candidates.size() && excess > 0;
             ++i, --excess) {
            const TileKey& victim = candidates[i].key;
            if (victim.z >= 0 &&
                victim.z < static_cast<int>(loadedPerZ_.size())) {
                --loadedPerZ_[static_cast<size_t>(victim.z)];
            }
            loaded_.erase(victim);
        }
    }

    Options options_;
    std::shared_ptr<TileScheme> scheme_;
    SchemeId schemeId_;

    std::unordered_map<TileKey, CachedTile> loaded_;
    /// 每层已加载瓦计数(R* 后代回退的早退闸:更深层无存货就不下探)。
    /// 与 loaded_ 同步维护(insert/erase 两处),32 层覆盖 WebMercator 全域。
    std::array<int, 32> loadedPerZ_{};
    std::unordered_set<TileKey> pending_;
    std::unordered_set<TileKey> failed_;
    uint64_t frame_ = 0;
};

/// MVT 消费方别名:载荷 = 解码后的 MVT 瓦片。语义与旧的
/// `VectorTileTree` 完全一致,调用方零改动。
using VectorTileTree = VectorTileTreeT<MvtTile>;

} // namespace earth_engine
