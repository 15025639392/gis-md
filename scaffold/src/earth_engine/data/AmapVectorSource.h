#pragma once

#include "AmapGeometry.h"
#include "Feature.h"
#include "MvtVectorSource.h"
#include "../tiling/TileKey.h"

#include <memory>
#include <string>
#include <vector>

namespace earth_engine {

/// 高德 Nebula 载荷 → 引擎 Feature 列表的恒等转换。
///
/// amap 的 DecodeTraits 在解码期已经完成「字节 → Feature 列表 + 按源过滤
/// (regionsOnly)」,worker 直接拿这份列表镶嵌,无需再经层规则。includeLayers/
/// layerRules 对 amap 语义为空(过滤在解码期按 type 做,不是 MVT 层名)。
struct AmapToFeatures {
    std::vector<Feature> operator()(
        const TileKey&, std::shared_ptr<const std::vector<Feature>> tile,
        const std::vector<std::string>&,
        const std::vector<SourceLayerRule>&) const {
        return *tile;
    }
};

/// amap POI 解码特质:字节流 → Feature 列表(点标签)。
/// 用 decodeAmapPoiTile 解码(type 0 通用 POI 点),经
/// amapDecodedPartToFeatures 转 Point Feature(name/rank/subKey)。
struct AmapPoiDecodeTraits {
    static bool decode(const uint8_t* data, size_t size,
                       std::vector<Feature>& out, std::string* error) {
        std::vector<AmapDecodedLayerPart> parts;
        if (!decodeAmapPoiTile(data, size, parts, error)) {
            return false;
        }
        for (const auto& p : parts) {
            if (p.type != 0) continue;  // 只取通用 POI 点层
            // [1:1 坐标空间] 与区域/路网一致:保留 GCJ 原生坐标对齐 amap.com。
            auto fs = amapDecodedPartToFeatures(p, false);
            out.insert(out.end(), std::make_move_iterator(fs.begin()),
                       std::make_move_iterator(fs.end()));
        }
        return true;
    }

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

/// amap 区域粗源(z10:水/绿地,type2)。
using AmapRegionsVectorSource = VectorTileSourceT<
    std::vector<Feature>, AmapDecodeTraits<true>, AmapToFeatures>;

/// amap 主源(z12-14:路网/建筑/轨道与地块,type1/2/3/4；30001 水/绿地
/// 由 z12 water source 唯一提供)。
using AmapMainVectorSource = VectorTileSourceT<
    std::vector<Feature>, AmapDecodeTraits<false>, AmapToFeatures>;

/// amap POI 源(z14:type 0 通用 POI 点标签)。
using AmapPoiVectorSource = VectorTileSourceT<
    std::vector<Feature>, AmapPoiDecodeTraits, AmapToFeatures>;

/// 高德瓦片地理矩形(弧度,4326 等距圆柱 2:1)。
/// 与 AmapGeographicScheme::tileToRectangle 同数学;GLESView 的
/// workerTessellationContextForArea 需要按瓦片矩形取高度范围。
Rectangle amapTileRectangle(const TileKey& key);

} // namespace earth_engine
