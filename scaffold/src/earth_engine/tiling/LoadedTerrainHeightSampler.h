#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace earth_engine {

struct TilesetTile;

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
