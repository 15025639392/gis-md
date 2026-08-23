#include "AmapVectorTile.h"

#include <zlib.h>

#include <cstring>
#include <limits>

namespace earth_engine {
namespace {

struct Reader {
    const uint8_t* p = nullptr;
    size_t n = 0;
    size_t i = 0;
    bool ok = true;

    uint64_t varint() {
        uint64_t v = 0;
        int shift = 0;
        while (i < n && shift <= 63) {
            const uint8_t b = p[i++];
            v |= static_cast<uint64_t>(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
            shift += 7;
        }
        ok = false;
        return 0;
    }

    // 返回 (field, wire_type);EOF 返回 false。
    bool tag(int& field, int& wire) {
        if (i >= n) return false;
        const uint64_t t = varint();
        if (!ok) return false;
        field = static_cast<int>(t >> 3);
        wire = static_cast<int>(t & 7);
        return true;
    }

    bool bytes(size_t len, const uint8_t*& out) {
        if (i + len > n) {
            ok = false;
            return false;
        }
        out = p + i;
        i += len;
        return true;
    }
};

bool inflateContainer(const uint8_t* data, size_t size,
                      std::vector<uint8_t>& out, std::string* error) {
    if (size < 5) {
        if (error) *error = "amap: container too small";
        return false;
    }
    const uint32_t declared =
        (static_cast<uint32_t>(data[0]) << 24) |
        (static_cast<uint32_t>(data[1]) << 16) |
        (static_cast<uint32_t>(data[2]) << 8) |
        static_cast<uint32_t>(data[3]);
    if (declared != size - 4) {
        if (error) {
            *error = "amap: length header mismatch (" +
                     std::to_string(declared) + " vs " +
                     std::to_string(size - 4) + ")";
        }
        return false;
    }
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, 32 + MAX_WBITS) != Z_OK) {
        if (error) *error = "amap: inflateInit2 failed";
        return false;
    }
    strm.next_in = const_cast<uint8_t*>(data + 4);
    strm.avail_in = static_cast<uInt>(size - 4);
    out.clear();
    std::vector<uint8_t> chunk(8192);
    int rc = Z_OK;
    while (rc == Z_OK) {
        strm.next_out = chunk.data();
        strm.avail_out = static_cast<uInt>(chunk.size());
        rc = inflate(&strm, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&strm);
            if (error) *error = "amap: inflate failed";
            return false;
        }
        const size_t produced = chunk.size() - strm.avail_out;
        out.insert(out.end(), chunk.begin(), chunk.begin() + produced);
    }
    inflateEnd(&strm);
    return true;
}

// 按 zigzag 解码(首点同样 zigzag 编码,随后是增量)。
int64_t zigzag(uint64_t v) {
    return static_cast<int64_t>(v >> 1) ^
           -static_cast<int64_t>(v & 1);
}

void decodeBlob(const uint8_t* blob, size_t len,
                std::vector<std::pair<double, double>>& ring) {
    Reader r;
    r.p = blob;
    r.n = len;
    int64_t x = 0, y = 0;
    while (r.i < r.n && r.ok) {
        const uint64_t dx = r.varint();
        if (!r.ok || r.i >= r.n) break;
        const uint64_t dy = r.varint();
        if (!r.ok) break;
        x += zigzag(dx);
        y += zigzag(dy);
        ring.emplace_back(static_cast<double>(x), static_cast<double>(y));
    }
}

void parseFeature(Reader& f, int classCode, int geomType, int layerType,
                  AmapDecodedFeature& feat) {
    feat.classCode = classCode;
    feat.geomType = geomType;
    // 几何所在 Part 字段按层类型区分(实测):type1 线 = Part{blob #5};
    // type3 建筑 / type4 轨道 = Part{blob #3};type2 区域 = Feature #6 环。
    const int partBlobField = (layerType == 1) ? 5 : 3;
    int field = 0, wire = 0;
    while (f.tag(field, wire)) {
        if (wire == 0) {
            (void)f.varint();
        } else if (wire == 2) {
            const uint64_t len = f.varint();
            if (!f.ok || len > std::numeric_limits<size_t>::max()) {
                f.ok = false;
                return;
            }
            const uint8_t* sub = nullptr;
            if (!f.bytes(static_cast<size_t>(len), sub)) return;
            if (field == 4) {
                // Part(线/建筑/轨道共用容器)。
                Reader pr;
                pr.p = sub;
                pr.n = static_cast<size_t>(len);
                int pf = 0, pw = 0;
                std::vector<std::pair<double, double>> ring;
                while (pr.tag(pf, pw)) {
                    if (pw == 0) {
                        const uint64_t v = pr.varint();
                        if (pf == 5 && feat.height == 0.0) {
                            feat.height = static_cast<double>(v);
                        }
                    } else if (pw == 2) {
                        const uint64_t plen = pr.varint();
                        if (!pr.ok ||
                            plen > std::numeric_limits<size_t>::max()) {
                            pr.ok = false;
                            break;
                        }
                        const uint8_t* psub = nullptr;
                        if (!pr.bytes(static_cast<size_t>(plen), psub)) break;
                        if (pf == partBlobField) {
                            decodeBlob(psub, static_cast<size_t>(plen), ring);
                        } else if (pf == 3 && partBlobField == 5) {
                            // type1 线:blob 在 #5;若有 #3(实测未见)忽略。
                        } else if (pf == 5 && layerType != 1) {
                            // 建筑高度 varint 已在上面 wire==0 分支读取;
                            // 此处兜底字符串不适用,忽略。
                            feat.name.assign(
                                reinterpret_cast<const char*>(psub),
                                static_cast<size_t>(plen));
                        }
                    }
                }
                if (!ring.empty()) feat.rings.push_back(std::move(ring));
            } else if (field == 6 && layerType == 2) {
                // type2 区域:Feature #6 为 repeated ring blob。
                std::vector<std::pair<double, double>> ring;
                decodeBlob(sub, static_cast<size_t>(len), ring);
                if (!ring.empty()) feat.rings.push_back(std::move(ring));
            } else if (field == 6) {
                // 名称(标签/道路名),字符串。
                feat.name.assign(reinterpret_cast<const char*>(sub),
                                 static_cast<size_t>(len));
            }
        } else {
            f.ok = false;
            return;
        }
    }
}

bool decodeContainer(const uint8_t* data, size_t size,
                     std::vector<AmapDecodedLayerPart>& out,
                     std::string* error) {
    std::vector<uint8_t> inflated;
    if (!inflateContainer(data, size, inflated, error)) return false;

    Reader root;
    root.p = inflated.data();
    root.n = inflated.size();
    int field = 0, wire = 0;
    while (root.tag(field, wire)) {
        if (wire != 2) {
            root.ok = false;
            break;
        }
        const uint64_t len = root.varint();
        if (!root.ok || len > std::numeric_limits<size_t>::max()) break;
        const uint8_t* tile = nullptr;
        if (!root.bytes(static_cast<size_t>(len), tile)) break;
        if (field != 1) continue;  // root: Tile #1;version #2 忽略

        Reader t;
        t.p = tile;
        t.n = static_cast<size_t>(len);
        int tf = 0, tw = 0;
        while (t.tag(tf, tw)) {
            if (tw != 2) {
                (void)t.varint();
                continue;
            }
            const uint64_t tlen = t.varint();
            if (!t.ok || tlen > std::numeric_limits<size_t>::max()) break;
            const uint8_t* layer = nullptr;
            if (!t.bytes(static_cast<size_t>(tlen), layer)) break;
            if (tf != 4) continue;  // Tile: repeated Layer #4

            Reader l;
            l.p = layer;
            l.n = static_cast<size_t>(tlen);
            AmapDecodedLayerPart part;
            const uint8_t* content = nullptr;
            size_t contentLen = 0;
            int lf = 0, lw = 0;
            while (l.tag(lf, lw)) {
                if (lw == 0) {
                    const uint64_t v = l.varint();
                    if (lf == 1) part.z = static_cast<int>(v);
                    if (lf == 2) part.x = static_cast<int>(v);
                    if (lf == 3) part.y = static_cast<int>(v);
                    if (lf == 4) part.type = static_cast<int>(v);
                } else if (lw == 2) {
                    const uint64_t clen = l.varint();
                    if (!l.ok || clen > std::numeric_limits<size_t>::max()) {
                        l.ok = false;
                        break;
                    }
                    const uint8_t* sub = nullptr;
                    if (!l.bytes(static_cast<size_t>(clen), sub)) break;
                    if (lf == 5) {
                        content = sub;
                        contentLen = static_cast<size_t>(clen);
                    }
                } else {
                    l.ok = false;
                    break;
                }
            }
            if (content) {
                Reader c;
                c.p = content;
                c.n = contentLen;
                int cf = 0, cw = 0;
                while (c.tag(cf, cw)) {
                    if (cw != 2) {
                        c.ok = false;
                        break;
                    }
                    const uint64_t clen = c.varint();
                    if (!c.ok || clen > std::numeric_limits<size_t>::max()) {
                        c.ok = false;
                        break;
                    }
                    const uint8_t* cg = nullptr;
                    if (!c.bytes(static_cast<size_t>(clen), cg)) break;
                    // ClassGroup:实测在 content 字段 1;放宽接受任一
                    // len 字段(参考文档未钉字段号)。
                    Reader g;
                    g.p = cg;
                    g.n = static_cast<size_t>(clen);
                    // classCode 只在字段 1(实测 type1=20009 在 #1);
                    // type2 区域 #1 缺省,默认 30001;type3 建筑合成 90001。
                    const int defaultClass = part.type == 1 ? 20004
                                             : part.type == 2 ? 30001
                                                               : 90001;
                    int classCode = defaultClass, geomType = 0;
                    int gf = 0, gw = 0;
                    std::vector<const uint8_t*> feats;
                    std::vector<size_t> featLens;
                    while (g.tag(gf, gw)) {
                        if (gw == 0) {
                            const uint64_t v = g.varint();
                            if (gf == 1) classCode = static_cast<int>(v);
                            if (gf == 2 || gf == 3) {
                                geomType = static_cast<int>(v);
                            }
                        } else if (gw == 2) {
                            const uint64_t flen = g.varint();
                            if (!g.ok ||
                                flen > std::numeric_limits<size_t>::max()) {
                                g.ok = false;
                                break;
                            }
                            const uint8_t* fs = nullptr;
                            if (!g.bytes(static_cast<size_t>(flen), fs)) break;
                            if (gf == 4) {
                                feats.push_back(fs);
                                featLens.push_back(static_cast<size_t>(flen));
                            }
                        }
                    }
                    for (size_t fi = 0; fi < feats.size(); ++fi) {
                        Reader fr;
                        fr.p = feats[fi];
                        fr.n = featLens[fi];
                        AmapDecodedFeature feat;
                        parseFeature(fr, classCode, geomType, part.type, feat);
                        if (!feat.rings.empty() || !feat.name.empty()) {
                            part.features.push_back(std::move(feat));
                        }
                    }
                }
            }
            out.push_back(std::move(part));
        }
    }
    return root.ok || !out.empty();
}

}  // namespace

bool decodeAmapTile(const uint8_t* data, size_t size,
                    std::vector<AmapDecodedLayerPart>& out,
                    std::string* error) {
    out.clear();
    return decodeContainer(data, size, out, error);
}

bool decodeAmapPoiTile(const uint8_t* data, size_t size,
                       std::vector<AmapDecodedLayerPart>& out,
                       std::string* error) {
    // POI 组与主组共享容器/层/类组结构;标签类几何(单点 unsigned)与
    // name 语义在 POI 专项接入时补全。
    return decodeAmapTile(data, size, out, error);
}

}  // namespace earth_engine
