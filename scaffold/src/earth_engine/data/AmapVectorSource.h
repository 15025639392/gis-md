#pragma once

#include "AmapGeometry.h"
#include "Feature.h"
#include "MvtVectorSource.h"
#include "../tiling/TileKey.h"

#include <memory>
#include <iterator>
#include <string>
#include <vector>

namespace earth_engine {

inline void appendAmapPartFeatures(const AmapDecodedLayerPart& part,
                                   std::vector<Feature>& out) {
    auto features = amapDecodedPartToFeatures(part, false);
    out.insert(out.end(), std::make_move_iterator(features.begin()),
               std::make_move_iterator(features.end()));
}

/// type1 完整解码载荷 → 粗区域 source。筛选放到消费阶段，让 regions/main/
/// water12 共享同一个 gzip/protobuf 解码结果。
struct AmapRegionsToFeatures {
    std::vector<Feature> operator()(
        const TileKey&, std::shared_ptr<const AmapDecodedTile> tile,
        const std::vector<std::string>&,
        const std::vector<SourceLayerRule>&) const {
        std::vector<Feature> out;
        for (const AmapDecodedLayerPart& part : tile->parts) {
            if (part.type == 2) appendAmapPartFeatures(part, out);
        }
        return out;
    }
};

/// type1 完整解码载荷 → main source。低档 type2 由 regions 唯一提供；
/// z12+ 只接非 30001 地块，水/绿仍由 water12 提供。
struct AmapMainToFeatures {
    std::vector<Feature> operator()(
        const TileKey&, std::shared_ptr<const AmapDecodedTile> tile,
        const std::vector<std::string>&,
        const std::vector<SourceLayerRule>&) const {
        std::vector<Feature> out;
        for (const AmapDecodedLayerPart& part : tile->parts) {
            if (part.type == 2) {
                if (part.z < 12) continue;
                AmapDecodedLayerPart kept = part;
                kept.features.clear();
                for (const AmapDecodedFeature& feature : part.features) {
                    if (feature.classCode != 30001) {
                        kept.features.push_back(feature);
                    }
                }
                appendAmapPartFeatures(kept, out);
                continue;
            }
            appendAmapPartFeatures(part, out);
        }
        return out;
    }
};

struct AmapWaterToFeatures {
    std::vector<Feature> operator()(
        const TileKey&, std::shared_ptr<const AmapDecodedTile> tile,
        const std::vector<std::string>&,
        const std::vector<SourceLayerRule>&) const {
        std::vector<Feature> out;
        for (const AmapDecodedLayerPart& part : tile->parts) {
            if (part.type != 2) continue;
            AmapDecodedLayerPart kept = part;
            kept.features.clear();
            for (const AmapDecodedFeature& feature : part.features) {
                if (feature.classCode == 30001) {
                    kept.features.push_back(feature);
                }
            }
            appendAmapPartFeatures(kept, out);
        }
        return out;
    }
};

/// POI 保留为轻量解码结构，进入 tessellation worker 后才转换 Feature。
/// 旧通路在 decode 阶段先构造 Feature，派单时 `return *tile` 又深复制整份
/// rings/properties；全球 64 瓦会产生一轮纯内存复制和分配抖动。
struct AmapPoiDecodedTileDecodeTraits {
    static bool decode(const uint8_t* data, size_t size,
                       AmapDecodedTile& out, std::string* error) {
        return decodeAmapPoiTile(data, size, out.parts, error);
    }

    static size_t approxBytes(const AmapDecodedTile& tile) {
        return AmapDecodedTileDecodeTraits::approxBytes(tile);
    }
};

struct AmapPoiToFeatures {
    std::vector<Feature> operator()(
        const TileKey&, std::shared_ptr<const AmapDecodedTile> tile,
        const std::vector<std::string>&,
        const std::vector<SourceLayerRule>&) const {
        std::vector<Feature> out;
        for (const AmapDecodedLayerPart& part : tile->parts) {
            if (part.type == 0) appendAmapPartFeatures(part, out);
        }
        return out;
    }
};

/// amap 解码特质:字节流 → Feature 列表。
/// RegionsOnly 编译期开关:粗源(z10/z12 区域)保留 type2 面,主源(z12-14)
/// 保留 type1/3/4 与 30002 地块面，过滤 30001 水/绿地——过滤在 worker
/// 解码期做,不进缓存。
template <bool RegionsOnly>
struct AmapDecodeTraits {
    static bool decode(const uint8_t* data, size_t size,
                       std::vector<Feature>& out, std::string* error) {
        return amapBytesToFeatures(data, size, RegionsOnly, out, error);
    }

    /// 解码瓦的近似常驻字节(几何点 + 属性字符串 + 容器头)。
    static size_t approxBytes(const std::vector<Feature>& feats) {
        constexpr size_t kMapNodeBytes = 56;
        constexpr size_t kVecHeaderBytes = 24;
        size_t bytes = sizeof(std::vector<Feature>) +
                       feats.capacity() * sizeof(Feature);
        for (const Feature& f : feats) {
            bytes += f.rings.capacity() * kVecHeaderBytes;
            for (const auto& ring : f.rings) {
                bytes += ring.capacity() * sizeof(Cartographic);
            }
            bytes += f.properties.size() * kMapNodeBytes;
            for (const auto& kv : f.properties) {
                if (kv.first.size() > 15) bytes += kv.first.size();
                if (kv.second.size() > 15) bytes += kv.second.size();
            }
        }
        return bytes;
    }
};

/// amap 区域粗源(z3/6/8/10:水/绿地与地块,type2)。
using AmapType1TileCache =
    MvtTileFetchCacheT<AmapDecodedTile, AmapDecodedTileDecodeTraits>;

using AmapRegionsVectorSource = VectorTileSourceT<
    AmapDecodedTile, AmapDecodedTileDecodeTraits, AmapRegionsToFeatures>;

using AmapWaterVectorSource = VectorTileSourceT<
    AmapDecodedTile, AmapDecodedTileDecodeTraits, AmapWaterToFeatures>;

/// amap 主源(z3/6/8/10/12/14:路网/建筑/轨道；z12+ 另含地块
/// type2。低档地块由 regions source 唯一提供，30001 水/绿地由粗区域/
/// z12 water source 提供)。
using AmapMainVectorSource = VectorTileSourceT<
    AmapDecodedTile, AmapDecodedTileDecodeTraits, AmapMainToFeatures>;

/// amap POI 源(z3/6/8/10/12/14:type 0 通用 POI 点标签)。
using AmapPoiVectorSource = VectorTileSourceT<
    AmapDecodedTile, AmapPoiDecodedTileDecodeTraits, AmapPoiToFeatures>;

/// 高德瓦片地理矩形(弧度,4326 等距圆柱 2:1)。
/// 与 AmapGeographicScheme::tileToRectangle 同数学;GLESView 的
/// workerTessellationContextForArea 需要按瓦片矩形取高度范围。
Rectangle amapTileRectangle(const TileKey& key);

} // namespace earth_engine
