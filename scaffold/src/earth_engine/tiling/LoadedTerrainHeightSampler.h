#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

struct TilesetTile;
class Rectangle;

// Batched terrain-height sampler over a fixed area. Gathers the candidate
// loaded-terrain tiles overlapping the area ONCE, then answers many point
// queries against just that set — avoiding the full-registry scan that
// sampleHeightOptional does per call. Used to lift the many vertices of a fill
// proxy (all inside the tile's rectangle) to loaded-terrain height cheaply.
class LoadedTerrainAreaSampler {
public:
    LoadedTerrainAreaSampler(
        const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>&
            tiles,
        const Rectangle& area);

    /// Height at (lon, lat) in radians from the deepest covering candidate, or
    /// nullopt if no loaded terrain in the area covers the point.
    std::optional<float> sample(double longitudeRadians,
                                double latitudeRadians) const;

    /// True if the area contains no loaded-terrain candidates at all (every
    /// sample would be nullopt — the whole proxy stays flat).
    bool empty() const { return candidates_.empty(); }

private:
    std::vector<const TilesetTile*> candidates_;
};

class LoadedTerrainHeightSampler {
public:
    // 返回 nullopt 表示"该点无任何已加载地形瓦片覆盖"(无数据),与真实海平面
    // 高度 0 明确区分——相机 clamp 需要这个区分来避免在未加载高地形处下沉。
    static std::optional<float> sampleHeightOptional(
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        double longitudeRadians,
        double latitudeRadians);

    // 便利包装:无数据回退海平面 0。拾取等"要一个具体高度、海平面兜底可接受"
    // 的调用方用此版本;相机 clamp 用 sampleHeightOptional 区分无数据。
    static float sampleHeight(
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        double longitudeRadians,
        double latitudeRadians) {
        return sampleHeightOptional(tiles, longitudeRadians, latitudeRadians)
            .value_or(0.0f);
    }
};

} // namespace earth_engine
