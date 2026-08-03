#include "MvtDecoder.h"

#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace earth_engine {

namespace {

// ---------------------------------------------------------------------------
// protobuf wire 解码(手写,规避 protobuf 库依赖;MVT 只用到
// varint / fixed32 / fixed64 / length-delimited 四种 wire type)
// ---------------------------------------------------------------------------

struct PbfReader {
    const uint8_t* cur = nullptr;
    const uint8_t* end = nullptr;
    bool ok = true;

    PbfReader(const uint8_t* data, size_t size) : cur(data), end(data + size) {}

    bool atEnd() const { return cur >= end; }

    uint64_t readVarint() {
        uint64_t value = 0;
        int shift = 0;
        while (cur < end && shift < 64) {
            uint8_t byte = *cur++;
            value |= static_cast<uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                return value;
            }
            shift += 7;
        }
        ok = false;
        return 0;
    }

    uint32_t readFixed32() {
        if (end - cur < 4) {
            ok = false;
            return 0;
        }
        uint32_t v;
        std::memcpy(&v, cur, 4);
        cur += 4;
        return v;  // 引擎全平台小端,与 wire 格式一致
    }

    uint64_t readFixed64() {
        if (end - cur < 8) {
            ok = false;
            return 0;
        }
        uint64_t v;
        std::memcpy(&v, cur, 8);
        cur += 8;
        return v;
    }

    /// length-delimited 字段:返回子区间并前进。
    PbfReader readSubMessage() {
        uint64_t len = readVarint();
        if (!ok || len > static_cast<uint64_t>(end - cur)) {
            ok = false;
            return PbfReader(nullptr, 0);
        }
        PbfReader sub(cur, static_cast<size_t>(len));
        cur += len;
        return sub;
    }

    std::string readString() {
        PbfReader sub = readSubMessage();
        if (!ok) {
            return {};
        }
        return std::string(reinterpret_cast<const char*>(sub.cur),
                           static_cast<size_t>(sub.end - sub.cur));
    }

    void skipField(uint32_t wireType) {
        switch (wireType) {
            case 0: readVarint(); break;
            case 1: readFixed64(); break;
            case 2: readSubMessage(); break;
            case 5: readFixed32(); break;
            default: ok = false; break;  // group(3/4)已废弃,MVT 不出现
        }
    }
};

inline int64_t zigzagDecode(uint64_t n) {
    return static_cast<int64_t>(n >> 1) ^ -static_cast<int64_t>(n & 1);
}

// ---------------------------------------------------------------------------
// Value 字符串化:最短往返表示(properties 统一存字符串,
// 对齐 Feature::properties / GeoJsonParser 的既有约定)
// ---------------------------------------------------------------------------

std::string doubleToShortestString(double v) {
    char buf[32];
    for (int precision = 6; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
        if (std::strtod(buf, nullptr) == v) {
            break;
        }
    }
    return buf;
}

/// Layer.values 元素(oneof 语义:先到先得,后出现的字段忽略)。
bool parseValueToString(PbfReader value, std::string& out) {
    while (!value.atEnd() && value.ok) {
        uint64_t tag = value.readVarint();
        if (!value.ok) {
            return false;
        }
        uint32_t field = static_cast<uint32_t>(tag >> 3);
        uint32_t wire = static_cast<uint32_t>(tag & 0x7);
        switch (field) {
            case 1:  // string_value
                out = value.readString();
                return value.ok;
            case 2: {  // float_value
                uint32_t bits = value.readFixed32();
                float f;
                std::memcpy(&f, &bits, 4);
                out = doubleToShortestString(static_cast<double>(f));
                return value.ok;
            }
            case 3: {  // double_value
                uint64_t bits = value.readFixed64();
                double d;
                std::memcpy(&d, &bits, 8);
                out = doubleToShortestString(d);
                return value.ok;
            }
            case 4:  // int_value
                out = std::to_string(static_cast<int64_t>(value.readVarint()));
                return value.ok;
            case 5:  // uint_value
                out = std::to_string(value.readVarint());
                return value.ok;
            case 6:  // sint_value
                out = std::to_string(zigzagDecode(value.readVarint()));
                return value.ok;
            case 7:  // bool_value
                out = value.readVarint() != 0 ? "true" : "false";
                return value.ok;
            default:
                value.skipField(wire);
                break;
        }
    }
    // 空 Value(合法但无内容)→ 空串
    out.clear();
    return value.ok;
}

// ---------------------------------------------------------------------------
// 几何命令流解码
// ---------------------------------------------------------------------------

constexpr uint32_t kCmdMoveTo = 1;
constexpr uint32_t kCmdLineTo = 2;
constexpr uint32_t kCmdClosePath = 7;

/// geometry 是 packed uint32 命令流:cmd_integer = (cmd & 0x7) | (count << 3)。
/// 坐标 zigzag+delta,游标跨路径持续累积(spec §4.3.5.2)。
/// 返回 false = 命令流残缺/非法,该 feature 应整体丢弃。
bool decodeGeometry(const std::vector<uint32_t>& geometry, MvtGeomType type,
                    std::vector<std::vector<MvtPoint>>& paths) {
    int64_t x = 0;
    int64_t y = 0;
    std::vector<MvtPoint> current;

    size_t k = 0;
    while (k < geometry.size()) {
        uint32_t cmdInteger = geometry[k++];
        uint32_t cmd = cmdInteger & 0x7;
        uint32_t count = cmdInteger >> 3;

        if (cmd == kCmdMoveTo || cmd == kCmdLineTo) {
            if (count == 0 || k + static_cast<size_t>(count) * 2 > geometry.size()) {
                return false;
            }
            for (uint32_t i = 0; i < count; ++i) {
                x += zigzagDecode(geometry[k++]);
                y += zigzagDecode(geometry[k++]);
                // 点要素全部点合并进一个 path;线/面每个 MoveTo 点开新路径
                // (count>1 的 MoveTo = 连续多次移动,每次都是新路径起点)
                if (cmd == kCmdMoveTo && type != MvtGeomType::Point &&
                    !current.empty()) {
                    paths.push_back(std::move(current));
                    current.clear();
                }
                current.push_back({static_cast<int32_t>(x), static_cast<int32_t>(y)});
            }
        } else if (cmd == kCmdClosePath) {
            if (type != MvtGeomType::Polygon) {
                return false;
            }
            // ClosePath 无参数;隐式闭合,不重复首点
            if (!current.empty()) {
                paths.push_back(std::move(current));
                current.clear();
            }
        } else {
            return false;
        }
    }

    if (!current.empty()) {
        paths.push_back(std::move(current));
    }
    return !paths.empty();
}

/// packed 或非 packed 的 repeated uint32(spec 用 packed,
/// 但 protobuf 解析器必须两种都收)。
bool readRepeatedUint32(PbfReader& reader, uint32_t wireType,
                        std::vector<uint32_t>& out) {
    if (wireType == 2) {
        PbfReader sub = reader.readSubMessage();
        if (!reader.ok) {
            return false;
        }
        while (!sub.atEnd()) {
            uint64_t v = sub.readVarint();
            if (!sub.ok) {
                return false;
            }
            out.push_back(static_cast<uint32_t>(v));
        }
        return true;
    }
    if (wireType == 0) {
        uint64_t v = reader.readVarint();
        if (!reader.ok) {
            return false;
        }
        out.push_back(static_cast<uint32_t>(v));
        return true;
    }
    return false;
}

struct RawFeature {
    uint64_t id = 0;
    MvtGeomType type = MvtGeomType::Unknown;
    std::vector<uint32_t> tags;
    std::vector<uint32_t> geometry;
};

bool parseFeature(PbfReader feature, RawFeature& out) {
    while (!feature.atEnd() && feature.ok) {
        uint64_t tag = feature.readVarint();
        if (!feature.ok) {
            return false;
        }
        uint32_t field = static_cast<uint32_t>(tag >> 3);
        uint32_t wire = static_cast<uint32_t>(tag & 0x7);
        switch (field) {
            case 1:  // id
                out.id = feature.readVarint();
                break;
            case 2:  // tags
                if (!readRepeatedUint32(feature, wire, out.tags)) {
                    return false;
                }
                break;
            case 3: {  // type
                uint64_t t = feature.readVarint();
                out.type = t <= 3 ? static_cast<MvtGeomType>(t) : MvtGeomType::Unknown;
                break;
            }
            case 4:  // geometry
                if (!readRepeatedUint32(feature, wire, out.geometry)) {
                    return false;
                }
                break;
            default:
                feature.skipField(wire);
                break;
        }
    }
    return feature.ok;
}

/// Layer 两遍解析:protobuf 字段顺序任意,features 可能先于
/// keys/values 出现,故第一遍收 keys/values/extent + feature 子区间,
/// 第二遍才解 feature(tags 索引此时才可解引用)。
bool parseLayer(PbfReader layer, MvtLayer& out) {
    struct Span {
        const uint8_t* data;
        size_t size;
    };
    std::vector<Span> featureSpans;
    std::vector<std::string> keys;
    std::vector<std::string> values;

    while (!layer.atEnd() && layer.ok) {
        uint64_t tag = layer.readVarint();
        if (!layer.ok) {
            return false;
        }
        uint32_t field = static_cast<uint32_t>(tag >> 3);
        uint32_t wire = static_cast<uint32_t>(tag & 0x7);
        switch (field) {
            case 1:  // name
                out.name = layer.readString();
                break;
            case 2: {  // features
                PbfReader sub = layer.readSubMessage();
                if (!layer.ok) {
                    return false;
                }
                featureSpans.push_back(
                    {sub.cur, static_cast<size_t>(sub.end - sub.cur)});
                break;
            }
            case 3:  // keys
                keys.push_back(layer.readString());
                break;
            case 4: {  // values
                PbfReader sub = layer.readSubMessage();
                if (!layer.ok) {
                    return false;
                }
                std::string v;
                if (!parseValueToString(sub, v)) {
                    return false;
                }
                values.push_back(std::move(v));
                break;
            }
            case 5:  // extent
                out.extent = static_cast<uint32_t>(layer.readVarint());
                break;
            case 15:  // version
                out.version = static_cast<uint32_t>(layer.readVarint());
                break;
            default:
                layer.skipField(wire);
                break;
        }
    }
    if (!layer.ok) {
        return false;
    }

    out.features.reserve(featureSpans.size());
    for (const Span& span : featureSpans) {
        RawFeature raw;
        if (!parseFeature(PbfReader(span.data, span.size), raw)) {
            return false;  // protobuf 层损坏 → 整瓦片失败
        }

        MvtFeature feature;
        feature.id = raw.id;
        feature.type = raw.type;
        // 几何命令非法只丢弃该 feature(容错取向同 osgearth/maplibre)
        if (!decodeGeometry(raw.geometry, raw.type, feature.paths)) {
            continue;
        }
        // tags = (keyIdx, valueIdx) 对;越界对跳过,奇数尾忽略
        for (size_t i = 0; i + 1 < raw.tags.size(); i += 2) {
            uint32_t keyIdx = raw.tags[i];
            uint32_t valueIdx = raw.tags[i + 1];
            if (keyIdx >= keys.size() || valueIdx >= values.size()) {
                continue;
            }
            feature.properties[keys[keyIdx]] = values[valueIdx];
        }
        out.features.push_back(std::move(feature));
    }
    return true;
}

// ---------------------------------------------------------------------------
// 压缩识别 + zlib 解压
// ---------------------------------------------------------------------------

bool looksCompressed(const uint8_t* data, size_t size) {
    if (size < 2) {
        return false;
    }
    if (data[0] == 0x1F && data[1] == 0x8B) {
        return true;  // gzip
    }
    // raw zlib:CMF=0x78 且 (CMF<<8|FLG) % 31 == 0。
    // pbf 首字节 0x78 = field 15 varint,Tile 层无此字段,误判风险可忽略。
    if (data[0] == 0x78 && (static_cast<uint32_t>(data[0]) * 256 + data[1]) % 31 == 0) {
        return true;
    }
    return false;
}

bool inflateBytes(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                  std::string* error) {
    z_stream strm{};
    // 32+MAX_WBITS = 自动识别 gzip/zlib 头
    if (inflateInit2(&strm, 32 + MAX_WBITS) != Z_OK) {
        if (error) {
            *error = "mvt: inflateInit2 failed";
        }
        return false;
    }
    strm.next_in = const_cast<Bytef*>(data);
    strm.avail_in = static_cast<uInt>(size);

    out.clear();
    out.resize(size * 4 + 1024);
    size_t total = 0;
    int rc = Z_OK;
    do {
        if (total == out.size()) {
            out.resize(out.size() * 2);
        }
        strm.next_out = out.data() + total;
        strm.avail_out = static_cast<uInt>(out.size() - total);
        rc = inflate(&strm, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&strm);
            if (error) {
                *error = "mvt: inflate failed rc=" + std::to_string(rc);
            }
            return false;
        }
        total = out.size() - strm.avail_out;
    } while (rc != Z_STREAM_END);

    inflateEnd(&strm);
    out.resize(total);
    return true;
}

} // namespace

bool decodeMvtTile(const uint8_t* data, size_t size, MvtTile& out,
                   std::string* error) {
    out.layers.clear();
    if (data == nullptr || size == 0) {
        if (error) {
            *error = "mvt: empty input";
        }
        return false;
    }

    std::vector<uint8_t> inflated;
    if (looksCompressed(data, size)) {
        if (!inflateBytes(data, size, inflated, error)) {
            return false;
        }
        data = inflated.data();
        size = inflated.size();
    }

    PbfReader tile(data, size);
    while (!tile.atEnd() && tile.ok) {
        uint64_t tag = tile.readVarint();
        if (!tile.ok) {
            break;
        }
        uint32_t field = static_cast<uint32_t>(tag >> 3);
        uint32_t wire = static_cast<uint32_t>(tag & 0x7);
        if (field == 3 && wire == 2) {  // layers
            PbfReader sub = tile.readSubMessage();
            if (!tile.ok) {
                break;
            }
            MvtLayer layer;
            if (!parseLayer(sub, layer)) {
                if (error) {
                    *error = "mvt: malformed layer";
                }
                return false;
            }
            out.layers.push_back(std::move(layer));
        } else {
            tile.skipField(wire);
        }
    }
    if (!tile.ok) {
        if (error) {
            *error = "mvt: malformed tile pbf";
        }
        return false;
    }
    return true;
}

int64_t mvtRingSignedArea2(const std::vector<MvtPoint>& ring) {
    int64_t sum = 0;
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const MvtPoint& p1 = ring[i];
        const MvtPoint& p2 = ring[j];
        sum += (static_cast<int64_t>(p2.x) - p1.x) *
               (static_cast<int64_t>(p1.y) + p2.y);
    }
    return sum;
}

std::vector<MvtPolygon> classifyMvtRings(
    const std::vector<std::vector<MvtPoint>>& rings) {
    std::vector<MvtPolygon> polygons;
    bool exteriorIsNegative = false;
    bool orientationLatched = false;

    for (const auto& ring : rings) {
        if (ring.size() < 3) {
            continue;
        }
        int64_t area2 = mvtRingSignedArea2(ring);
        if (area2 == 0) {
            continue;
        }
        if (!orientationLatched) {
            exteriorIsNegative = area2 < 0;
            orientationLatched = true;
        }
        if ((area2 < 0) == exteriorIsNegative) {
            polygons.push_back({ring, {}});
        } else if (!polygons.empty()) {
            polygons.back().holes.push_back(ring);
        }
        // 反向环但尚无外环:孤儿孔,丢弃(osgearth 同)
    }
    return polygons;
}

} // namespace earth_engine
