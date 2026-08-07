#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

struct TilesetTile;
class Rectangle;

// ⚠️ 迁移 golden(生产勿用):LoadedTerrainAreaSampler 与
// LoadedTerrainHeightSampler 的生产调用方已全部迁往 TerrainHeightService
// (索引化点查询,语义逐点等价)。这两个类仅作对拍守卫的 golden 基准保留
// (test_terrain_height_service.cpp)——golden 必须是被冻结的旧实现,不能
// 跟着新实现演化。TerrainAncestorHeightSource 不在此列:它是"整面单一
// 祖先源"语义(fill 代理网格要一张连续面,不能逐点混档),仍是生产路径。
//
// Batched terrain-height sampler over a fixed area. Gathers the candidate
// loaded-terrain tiles overlapping the area ONCE, then answers many point
// queries against just that set — avoiding the full-registry scan that
// sampleHeightOptional does per call.
class LoadedTerrainAreaSampler {
public:
    /// @param renderGridConsistent true = 按渲染网格分段线性取高
    ///        (DecodedHeightmapSampler::sampleHeightRenderGrid,矢量贴地用:
    ///        全分辨率采样与渲染面的格内起伏差是结构性穿插来源);
    ///        false = 全分辨率双线性(fill 代理抬升等既有语义)。
    LoadedTerrainAreaSampler(
        const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>&
            tiles,
        const Rectangle& area,
        bool renderGridConsistent = false);

    /// Height at (lon, lat) in radians from the deepest covering candidate, or
    /// nullopt if no loaded terrain in the area covers the point.
    std::optional<float> sample(double longitudeRadians,
                                double latitudeRadians) const;

    /// True if the area contains no loaded-terrain candidates at all (every
    /// sample would be nullopt — the whole proxy stays flat).
    bool empty() const { return candidates_.empty(); }

private:
    std::vector<const TilesetTile*> candidates_;
    bool renderGridConsistent_ = false;
};

// 沿 parent 链取"最近的、带 retained heightmap 的地形祖先"作为高度来源。
// fill 代理用它把自己从海平面抬到粗版真实地形面(cesium TerrainFillMesh 在没有
// 可用邻居时同样回落到祖先高度)。
//
// 为什么是祖先而不是邻居:sample(lon,lat) 只能取"覆盖该点"的瓦片,而同级邻居
// 与本瓦片矩形相邻、不相交 —— 除边界线外根本不覆盖代理的任何顶点。真正覆盖
// 代理全域的只有祖先链,而祖先指针本就挂在 tile 上,无需扫瓦片注册表。
class TerrainAncestorHeightSource {
public:
    /// 最近的可用高度来源祖先,没有则 nullptr。O(树深)。
    static const TilesetTile* find(const TilesetTile& tile);

    /// 从指定来源瓦片取 (lon, lat) 的高度;该点不在其 heightmap 内则 nullopt。
    static std::optional<float> sample(
        const TilesetTile& source,
        double longitudeRadians,
        double latitudeRadians);
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
