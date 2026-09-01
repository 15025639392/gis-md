#include "AmapVectorTile.h"
#include "AmapVectorSourceInternal.h"

#include <zlib.h>

#include <algorithm>
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

bool assignNonnegativeInt(uint64_t value, int& target, Reader& reader) {
    if (value > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        reader.ok = false;
        return false;
    }
    target = static_cast<int>(value);
    return true;
}

void decodeBlob(const uint8_t* blob, size_t len,
                std::vector<std::pair<double, double>>& ring) {
    Reader r;
    r.p = blob;
    r.n = len;
    int64_t x = 0, y = 0;
    // Geometry blobs use an absolute zigzag-encoded first point followed by
    // zigzag-encoded deltas.  The protobuf framing of type2 Feature#6 is
    // handled by parseFeature; it must never be fed into this cursor.
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
                  AmapDecodedFeature& feat, bool lineGeometry = false,
                  bool polygonGeometry = false) {
    feat.classCode = classCode;
    feat.geomType = polygonGeometry ? 3 : (lineGeometry ? 2 : geomType);
    feat.lineGeometry = lineGeometry;
    feat.polygonGeometry = polygonGeometry;
    // 几何所在 Part 字段按层类型区分(实测):type1 线 = Part{blob #5};
    // type3 建筑 / type4 content.#1 轨道 = Part{blob #3};
    // type2 与 type4 content.#3 区域 = Feature #6 环。
    const int partBlobField = (layerType == 1) ? 5 : 3;
    int field = 0, wire = 0;
    while (f.tag(field, wire)) {
        if (wire == 0) {
            const uint64_t v = f.varint();
            // 区域:Feature #4 varint = primary kind。实测 z10 粗源
            // 61=绿地、63=水系,30002 地块 20/23 等;大区域 60/80 走
            // line-grid(见 amapCoordScale)。参考 xinzhi-map
            // decodeRegionFeature 的 #4 kind / #1 rank。
            if ((layerType == 2 || polygonGeometry) && field == 1 &&
                feat.rank == 0) {
                assignNonnegativeInt(v, feat.rank, f);
            }
            if ((layerType == 2 || polygonGeometry) && field == 4 &&
                feat.kind == 0) {
                assignNonnegativeInt(v, feat.kind, f);
            }
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
                        if (pf == 5 && !feat.hasHeight) {
                            // ShapeFeature.mainBuildingHeight is protobuf
                            // int32, not uint64 and not sint32. Negative int32
                            // values are therefore encoded as a ten-byte,
                            // sign-extended varint. Narrow through uint32_t
                            // before interpreting the sign so malformed or
                            // negative provider values cannot become enormous
                            // positive extrusion heights.
                            feat.height = static_cast<double>(
                                static_cast<int32_t>(
                                    static_cast<uint32_t>(v)));
                            feat.hasHeight = true;
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
                            // Building ShapeFeature #5 is the official varint
                            // mainBuildingHeight; length-delimited #5 is not a
                            // second compatible representation.
                            feat.name.assign(
                                reinterpret_cast<const char*>(psub),
                                static_cast<size_t>(plen));
                        }
                    }
                }
                if (!ring.empty()) feat.rings.push_back(std::move(ring));
            } else if (field == 6 && (layerType == 2 || polygonGeometry)) {
                // 区域 Feature #6 是一个嵌套 protobuf 消息:
                //   repeated field #1 (wire=2) = geometry ring blob.
                // 不能把 #6 的 tag/length 当成坐标，也不能把多个 #1
                // payload 串到同一个 cursor；每个 blob 都必须从自己的
                // 绝对首点独立解码。
                Reader rings;
                rings.p = sub;
                rings.n = static_cast<size_t>(len);
                int rf = 0, rw = 0;
                while (rings.tag(rf, rw)) {
                    if (rw == 2) {
                        const uint64_t rlen = rings.varint();
                        if (!rings.ok ||
                            rlen > std::numeric_limits<size_t>::max()) {
                            rings.ok = false;
                            break;
                        }
                        const uint8_t* rsub = nullptr;
                        if (!rings.bytes(static_cast<size_t>(rlen), rsub)) {
                            break;
                        }
                        if (rf != 1) continue;
                        std::vector<std::pair<double, double>> ring;
                        decodeBlob(rsub, static_cast<size_t>(rlen), ring);
                        if (!ring.empty()) feat.rings.push_back(std::move(ring));
                    } else if (rw == 0) {
                        (void)rings.varint();
                    } else {
                        rings.ok = false;
                        break;
                    }
                }
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

bool decodeInflatedContainer(const uint8_t* data, size_t size,
                             std::vector<AmapDecodedLayerPart>& out,
                             std::string*) {
    Reader root;
    root.p = data;
    root.n = size;
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
                    if (lf == 1) assignNonnegativeInt(v, part.z, l);
                    if (lf == 2) assignNonnegativeInt(v, part.x, l);
                    if (lf == 3) assignNonnegativeInt(v, part.y, l);
                    if (lf == 4) assignNonnegativeInt(v, part.type, l);
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
                    // Initialize from the official protobuf message defaults;
                    // an explicitly encoded zero still overwrites them and is
                    // rejected downstream. This is the provider contract, not
                    // a local compatibility fallback.
                    Reader g;
                    g.p = cg;
                    g.n = static_cast<size_t>(clen);
                    const bool boundaryGroup = part.type == 2 && cf == 2;
                    const bool type4RegionGroup = part.type == 4 && cf == 3;
                    int classCode =
                        part.type == 3
                            ? 55001
                            : (boundaryGroup ? 20016 : 0);
                    if (part.type == 1) classCode = 20004;
                    if (part.type == 2 && !boundaryGroup) classCode = 30001;
                    if (part.type == 4 && !type4RegionGroup) classCode = 20016;
                    if (type4RegionGroup) classCode = 30001;
                    int geomType = 0;
                    int kind = 0;
                    int subKey = 1;
                    int buildingResolution = part.type == 3 ? 12 : 0;
                    int gf = 0, gw = 0;
                    std::vector<const uint8_t*> feats;
                    std::vector<size_t> featLens;
                    while (g.tag(gf, gw)) {
                        if (gw == 0) {
                            const uint64_t v = g.varint();
                            if (gf == 1) assignNonnegativeInt(v, classCode, g);
                            if (gf == 2) {
                                assignNonnegativeInt(v, kind, g);
                                subKey = kind;
                            }
                            if (part.type == 3 && gf == 3) {
                                // Keep malformed unsigned protobuf values out
                                // of the signed coordinate contract. The
                                // adapter performs the z-dependent official
                                // shift validation and fails the group closed.
                                buildingResolution =
                                    v <= static_cast<uint64_t>(
                                             std::numeric_limits<int>::max())
                                        ? static_cast<int>(v)
                                        : 0;
                            } else if (gf == 2 || gf == 3) {
                                assignNonnegativeInt(v, geomType, g);
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
                        if (part.type == 3) {
                            feat.minZoom = 15;
                            feat.maxZoom = 30;
                            feat.drawOrder = 0;
                            feat.hasMinZoom = true;
                            feat.hasMaxZoom = true;
                            feat.hasDrawOrder = true;
                            feat.buildingResolution = buildingResolution;
                        } else {
                            feat.minZoom = 2;
                            feat.maxZoom = 30;
                            feat.drawOrder = 0;
                            feat.hasMinZoom = true;
                            feat.hasMaxZoom = true;
                            feat.hasDrawOrder = true;
                        }
                        // ClassGroup #4 is a *FeatureMulti wrapper.  The old
                        // decoder relied on its nested #4 layout to reach the
                        // geometry but silently discarded the wrapper's
                        // official drawOrder.  Preserve it before parsing the
                        // nested feature; runtime assigns this field directly
                        // to zIndex and sorts ascending.
                        {
                            Reader multi = fr;
                            int mf = 0, mw = 0;
                            while (multi.tag(mf, mw)) {
                                if (mw == 0) {
                                    const uint64_t v = multi.varint();
                                    if (mf == 1) {
                                        assignNonnegativeInt(v, feat.minZoom,
                                                             multi);
                                        feat.hasMinZoom = true;
                                    } else if (mf == 2) {
                                        assignNonnegativeInt(v, feat.maxZoom,
                                                             multi);
                                        feat.hasMaxZoom = true;
                                    } else if (mf == 3) {
                                        assignNonnegativeInt(v, feat.drawOrder,
                                                             multi);
                                        feat.hasDrawOrder = true;
                                    }
                                } else if (mw == 2) {
                                    const uint64_t skip = multi.varint();
                                    const uint8_t* ignored = nullptr;
                                    if (!multi.bytes(static_cast<size_t>(skip),
                                                     ignored)) {
                                        break;
                                    }
                                } else {
                                    multi.ok = false;
                                    break;
                                }
                            }
                        }
            // 区域的 kind 在 **Feature #4 varint**(参考
            // xinzhi-map decodeRegionFeature),ClassGroup #2 的
            // kind 不是同一语义(实测 ClassGroup #2 得 3/5,
            // Feature #4 得 61/63/20/23)。type2 不预置,由
            // parseFeature 读 Feature #4;其余类型保留 ClassGroup #2
            // 值(建筑 cat/轨道 subKey)。
                        feat.subKey = subKey;
                        if (boundaryGroup && classCode == 20017) {
                            // Building-level detail lines use a dedicated
                            // 16384x8192 grid (real z14 samples hit both exact
                            // maxima), unlike ordinary 4096x2048 boundaries.
                            feat.coordScale = 0.5;
                        }
                        if ((part.type != 2 && !type4RegionGroup) ||
                            boundaryGroup) {
                            feat.kind = kind;
                        }
                        const bool lineGeometry =
                            boundaryGroup || part.type == 1 ||
                            (part.type == 4 && !type4RegionGroup);
                        parseFeature(fr, classCode, geomType, part.type, feat,
                                     lineGeometry, type4RegionGroup);
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

bool decodeContainer(const uint8_t* data, size_t size,
                     std::vector<AmapDecodedLayerPart>& out,
                     std::string* error) {
    std::vector<uint8_t> inflated;
    if (!inflateContainer(data, size, inflated, error)) return false;
    return decodeInflatedContainer(inflated.data(), inflated.size(), out,
                                   error);
}

}  // namespace

bool AmapClassicSourceBundle::Impl::decodeType1(
    const uint8_t* data, size_t size, DecodedTile& out,
    std::string* error) {
    out.parts.clear();
    return decodeContainer(data, size, out.parts, error);
}

bool AmapClassicSourceBundle::Impl::decodePoi(
    const uint8_t* data, size_t size, DecodedTile& tile,
    std::string* error) {
    auto& out = tile.parts;
    // POI 组(参考 xinzhi-map decodeAmapPoiTile):
    //   type 0 = 通用 POI 点标签层;type 4 = 轨道线 + 站点标签;
    //   type 1 = 路名;type 2 = 边界线。
    // type 0 走 POI 标签解码;其余类型委托主解码(decodeAmapTile 已覆盖
    // type 1/2/3/4,保持 PoiEntrySharesContainerPath 等既有语义)。
    std::vector<uint8_t> inflated;
    if (!inflateContainer(data, size, inflated, error)) return false;

    // POI has its own official PointFeature/RoadLine schema. Do not run the
    // generic polygon/building decoder first and then erase its bogus output:
    // that was a second, non-authoritative decode path with avoidable geometry
    // allocation. Parse type 0/1 directly from the already-inflated payload.
    out.clear();

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
        if (field != 1) continue;  // root: Tile #1

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
                    if (lf == 1) assignNonnegativeInt(v, part.z, l);
                    if (lf == 2) assignNonnegativeInt(v, part.x, l);
                    if (lf == 3) assignNonnegativeInt(v, part.y, l);
                    if (lf == 4) assignNonnegativeInt(v, part.type, l);
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
            // type 1 is the dedicated road-name payload, not ordinary road
            // geometry. Preserve the payload's real road class/subKey and
            // mark only its non-visual geometry role; never synthesize a
            // local synthetic style identity.
            if (content && part.type == 1) {
                AmapDecodedLayerPart roadNames = part;
                roadNames.features.clear();
                Reader c;
                c.p = content;
                c.n = contentLen;
                int cf = 0, cw = 0;
                while (c.tag(cf, cw)) {
                    if (cw != 2) {
                        (void)c.varint();
                        continue;
                    }
                    const uint64_t glen = c.varint();
                    if (!c.ok || glen > std::numeric_limits<size_t>::max()) break;
                    const uint8_t* gb = nullptr;
                    if (!c.bytes(static_cast<size_t>(glen), gb)) break;
                    Reader g;
                    g.p = gb;
                    g.n = static_cast<size_t>(glen);
                    int roadClass = 20004, subKey = 1;
                    int gf = 0, gw = 0;
                    while (g.tag(gf, gw)) {
                        if (gw == 0) {
                            const uint64_t v = g.varint();
                            if (gf == 1) assignNonnegativeInt(v, roadClass, g);
                            if (gf == 2) assignNonnegativeInt(v, subKey, g);
                            continue;
                        }
                        if (gw != 2) {
                            g.ok = false;
                            break;
                        }
                        const uint64_t mlen = g.varint();
                        if (!g.ok || mlen > std::numeric_limits<size_t>::max()) break;
                        const uint8_t* mb = nullptr;
                        if (!g.bytes(static_cast<size_t>(mlen), mb)) break;
                        if (gf != 4) continue;
                        Reader multi;
                        multi.p = mb;
                        multi.n = static_cast<size_t>(mlen);
                        int minZoom = 2, maxZoom = 30, drawOrder = 0;
                        bool hasMinZoom = true, hasMaxZoom = true;
                        bool hasDrawOrder = true;
                        int mf = 0, mw = 0;
                        while (multi.tag(mf, mw)) {
                            if (mw == 0) {
                                const uint64_t v = multi.varint();
                                if (mf == 1) {
                                    assignNonnegativeInt(v, minZoom, multi);
                                    hasMinZoom = true;
                                }
                                if (mf == 2) {
                                    assignNonnegativeInt(v, maxZoom, multi);
                                    hasMaxZoom = true;
                                }
                                if (mf == 3) {
                                    assignNonnegativeInt(v, drawOrder, multi);
                                    hasDrawOrder = true;
                                }
                                continue;
                            }
                            if (mw != 2) {
                                multi.ok = false;
                                break;
                            }
                            const uint64_t plen = multi.varint();
                            if (!multi.ok || plen > std::numeric_limits<size_t>::max()) break;
                            const uint8_t* pb = nullptr;
                            if (!multi.bytes(static_cast<size_t>(plen), pb)) break;
                            if (mf != 4) continue;
                            Reader line;
                            line.p = pb;
                            line.n = static_cast<size_t>(plen);
                            AmapDecodedFeature feat;
                            feat.classCode = roadClass;
                            feat.geomType = 2;
                            feat.lineGeometry = true;
                            feat.roadNameGeometry = true;
                            feat.subKey = subKey;
                            feat.minZoom = minZoom;
                            feat.maxZoom = maxZoom;
                            feat.drawOrder = drawOrder;
                            feat.hasMinZoom = hasMinZoom;
                            feat.hasMaxZoom = hasMaxZoom;
                            feat.hasDrawOrder = hasDrawOrder;
                            int pf = 0, pw = 0;
                            while (line.tag(pf, pw)) {
                                if (pw == 0) {
                                    const uint64_t v = line.varint();
                                    if (pf == 2)
                                        assignNonnegativeInt(v, feat.rank, line);
                                    if (pf == 4)
                                        assignNonnegativeInt(v, feat.shieldType,
                                                             line);
                                    continue;
                                }
                                if (pw != 2) {
                                    line.ok = false;
                                    break;
                                }
                                const uint64_t flen = line.varint();
                                if (!line.ok || flen > std::numeric_limits<size_t>::max()) break;
                                const uint8_t* fb = nullptr;
                                if (!line.bytes(static_cast<size_t>(flen), fb)) break;
                                if (pf == 3) {
                                    feat.shield.assign(
                                        reinterpret_cast<const char*>(fb),
                                        static_cast<size_t>(flen));
                                } else if (pf == 1) {
                                    Reader nameBox;
                                    nameBox.p = fb;
                                    nameBox.n = static_cast<size_t>(flen);
                                    int nf = 0, nw = 0;
                                    while (nameBox.tag(nf, nw)) {
                                        if (nf == 1 && nw == 2) {
                                            const uint64_t slen = nameBox.varint();
                                            const uint8_t* sb = nullptr;
                                            if (!nameBox.bytes(static_cast<size_t>(slen), sb)) break;
                                            feat.name.assign(reinterpret_cast<const char*>(sb),
                                                             static_cast<size_t>(slen));
                                        } else if (nw == 0) {
                                            (void)nameBox.varint();
                                        } else {
                                            nameBox.ok = false;
                                            break;
                                        }
                                    }
                                } else if (pf == 5) {
                                    std::vector<std::pair<double, double>> ring;
                                    decodeBlob(fb, static_cast<size_t>(flen), ring);
                                    if (!ring.empty()) feat.rings.push_back(std::move(ring));
                                }
                            }
                            if ((!feat.name.empty() || !feat.shield.empty()) &&
                                !feat.rings.empty()) {
                                roadNames.features.push_back(std::move(feat));
                            }
                        }
                    }
                }
                if (!roadNames.features.empty()) out.push_back(std::move(roadNames));
                continue;
            }

            // PoiLayer(type 0) content.#1 and TransitLayer(type 4)
            // content.#2 share PointFeatureSameStyle. The official runtime
            // sends type-4 points to subwayLabel; do not discard them or run
            // them through the type-4 line decoder.
            if (!content || (part.type != 0 && part.type != 4)) {
                if (part.type == 0 || part.type == 4) {
                    out.push_back(std::move(part));
                }
                continue;
            }

            // content → repeated ClassGroup(任意 LEN 字段)。
            Reader c;
            c.p = content;
            c.n = contentLen;
            int cf = 0, cw = 0;
            while (c.tag(cf, cw)) {
                if (cw != 2) {
                    (void)c.varint();
                    continue;
                }
                const uint64_t clen = c.varint();
                if (!c.ok || clen > std::numeric_limits<size_t>::max()) break;
                const uint8_t* cg = nullptr;
                if (!c.bytes(static_cast<size_t>(clen), cg)) break;
                if (part.type == 4 && cf != 2) continue;
                if (part.type == 0 && cf != 1) continue;

                // PointFeatureSameStyle official protobuf defaults.
                Reader g;
                g.p = cg;
                g.n = static_cast<size_t>(clen);
                int classCode = 12024, subKey = 1, resolution = 12;
                std::vector<const uint8_t*> feats;
                std::vector<size_t> featLens;
                int gf = 0, gw = 0;
                while (g.tag(gf, gw)) {
                    if (gw == 0) {
                        const uint64_t v = g.varint();
                        if (gf == 1) assignNonnegativeInt(v, classCode, g);
                        if (gf == 2) assignNonnegativeInt(v, subKey, g);
                        if (gf == 3) assignNonnegativeInt(v, resolution, g);
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
                    } else {
                        g.ok = false;
                        break;
                    }
                }
                if (!g.ok || classCode == 0 || subKey == 0 ||
                    resolution < 0 || resolution > 30) continue;
                for (size_t fi = 0; fi < feats.size(); ++fi) {
                    Reader fr;
                    fr.p = feats[fi];
                    fr.n = featLens[fi];
                    // Feature:PointFeatureMulti { minZoom #1, maxZoom #2,
                    // rank #3, repeated label #4 }。
                    int minZoom = 18, maxZoom = 30, rank = 0;
                    bool hasMinZoom = true, hasMaxZoom = true;
                    std::vector<std::pair<const uint8_t*, size_t>> labels;
                    int ff = 0, fw = 0;
                    while (fr.tag(ff, fw)) {
                        if (fw == 0) {
                            const uint64_t v = fr.varint();
                            if (ff == 1) {
                                assignNonnegativeInt(v, minZoom, fr);
                                hasMinZoom = true;
                            }
                            if (ff == 2) {
                                assignNonnegativeInt(v, maxZoom, fr);
                                hasMaxZoom = true;
                            }
                            if (ff == 3) assignNonnegativeInt(v, rank, fr);
                        } else if (fw == 2) {
                            const uint64_t ll = fr.varint();
                            if (!fr.ok ||
                                ll > std::numeric_limits<size_t>::max()) {
                                fr.ok = false;
                                break;
                            }
                            const uint8_t* sub = nullptr;
                            if (!fr.bytes(static_cast<size_t>(ll), sub)) break;
                            if (ff == 4) labels.emplace_back(sub, static_cast<size_t>(ll));
                        } else {
                            fr.ok = false;
                            break;
                        }
                    }
                    // 每个 label 一个 POI 点。
                    for (const auto& [lb, ll] : labels) {
                        Reader lr;
                        lr.p = lb;
                        lr.n = ll;
                        AmapDecodedFeature feat;
                        feat.classCode = classCode;
                        feat.kind = subKey;
                        feat.subKey = subKey;
                        feat.minZoom = minZoom;
                        feat.maxZoom = maxZoom;
                        feat.hasMinZoom = hasMinZoom;
                        feat.hasMaxZoom = hasMaxZoom;
                        // PointFeature.drawOrder has the official protobuf
                        // default 0. Omission is semantically present and must
                        // not be confused with a malformed/missing local key.
                        feat.drawOrder = 0;
                        feat.hasDrawOrder = true;
                        feat.rank = rank;
                        feat.geomType = 1;  // Point
                        feat.pointGeometry = true;
                        feat.coordScale = std::ldexp(1.0, 14 - resolution);
                        int lf2 = 0, lw2 = 0;
                        int nf = 0, nw = 0;
                        const uint8_t* sb = nullptr;
                        std::pair<double, double> anchor;
                        bool hasAnchor = false;
                        bool selectedNameLoc = false;
                        while (lr.tag(lf2, lw2)) {
                            if (lw2 == 0) {
                                const uint64_t v = lr.varint();
                                if (lf2 == 2) {
                                    assignNonnegativeInt(v, feat.drawOrder, lr);
                                    feat.hasDrawOrder = true;
                                }
                                if (lf2 == 6) {
                                    feat.uid = v;
                                }
                                continue;
                            }
                            if (lf2 == 1 && lw2 == 2) {
                                // repeated nameLoc Language { name #1,
                                // repeated Mii #4 }. Official default s6t
                                // selects nameLoc[0], not the last occurrence.
                                const uint64_t nl = lr.varint();
                                if (!lr.ok ||
                                    nl > std::numeric_limits<size_t>::max()) {
                                    lr.ok = false;
                                    break;
                                }
                                const uint8_t* nb = nullptr;
                                if (!lr.bytes(static_cast<size_t>(nl), nb)) break;
                                Reader nr;
                                nr.p = nb;
                                nr.n = static_cast<size_t>(nl);
                                std::string candidateName;
                                std::vector<uint32_t> candidateSplitIndices;
                                while (nr.tag(nf, nw)) {
                                    if (nf == 1 && nw == 2) {
                                        const uint64_t sl = nr.varint();
                                        if (!nr.bytes(static_cast<size_t>(sl), sb)) break;
                                        candidateName.assign(
                                            reinterpret_cast<const char*>(sb),
                                            static_cast<size_t>(sl));
                                    } else if (nf == 4 && nw == 0) {
                                        const uint64_t boundary = nr.varint();
                                        if (boundary >
                                            std::numeric_limits<uint32_t>::max()) {
                                            nr.ok = false;
                                            break;
                                        }
                                        candidateSplitIndices.push_back(
                                            static_cast<uint32_t>(boundary));
                                    } else if (nf == 4 && nw == 2) {
                                        const uint64_t packedLength = nr.varint();
                                        if (!nr.ok || packedLength >
                                            std::numeric_limits<size_t>::max()) {
                                            nr.ok = false;
                                            break;
                                        }
                                        const uint8_t* packed = nullptr;
                                        if (!nr.bytes(
                                                static_cast<size_t>(packedLength),
                                                packed)) break;
                                        Reader pr;
                                        pr.p = packed;
                                        pr.n = static_cast<size_t>(packedLength);
                                        while (pr.i < pr.n) {
                                            const uint64_t boundary = pr.varint();
                                            if (!pr.ok || boundary >
                                                std::numeric_limits<uint32_t>::max()) {
                                                pr.ok = false;
                                                break;
                                            }
                                            candidateSplitIndices.push_back(
                                                static_cast<uint32_t>(boundary));
                                        }
                                        if (!pr.ok) nr.ok = false;
                                    } else if (nw == 2) {
                                        const uint64_t sl = nr.varint();
                                        sb = nullptr;
                                        if (!nr.bytes(static_cast<size_t>(sl), sb)) break;
                                    } else if (nw == 0) {
                                        (void)nr.varint();
                                    } else {
                                        nr.ok = false;
                                        break;
                                    }
                                }
                                if (!nr.ok) {
                                    lr.ok = false;
                                    break;
                                }
                                if (!selectedNameLoc) {
                                    feat.name = std::move(candidateName);
                                    feat.nameSplitIndicesUtf16 =
                                        std::move(candidateSplitIndices);
                                    selectedNameLoc = true;
                                }
                            } else if (lf2 == 4 && lw2 == 2) {
                                // 坐标 blob:单点 plain unsigned(x 后 y,
                                // **无 protobuf tag**,参考 zigzagFirst=false)。
                                const uint64_t gl = lr.varint();
                                if (!lr.ok ||
                                    gl > std::numeric_limits<size_t>::max()) {
                                    lr.ok = false;
                                    break;
                                }
                                const uint8_t* gb = nullptr;
                                if (!lr.bytes(static_cast<size_t>(gl), gb)) break;
                                Reader gr;
                                gr.p = gb;
                                gr.n = static_cast<size_t>(gl);
                                if (gr.i < gr.n) {
                                    const uint64_t vx = gr.varint();
                                    const uint64_t vy =
                                        gr.i < gr.n ? gr.varint() : 0;
                                    anchor = {static_cast<double>(vx),
                                              static_cast<double>(vy)};
                                    hasAnchor = true;
                                }
                            } else if (lw2 == 2) {
                                const uint64_t sl = lr.varint();
                                sb = nullptr;
                                if (!lr.bytes(static_cast<size_t>(sl), sb)) break;
                            } else if (lw2 == 0) {
                                (void)lr.varint();
                            } else {
                                lr.ok = false;
                                break;
                            }
                        }
                        if (!lr.ok || !hasAnchor) continue;
                        feat.rings = {{{anchor.first, anchor.second}}};
                        part.features.push_back(std::move(feat));
                    }
                }
            }
            out.push_back(std::move(part));
        }
    }
    return true;
}
#if defined(EARTH_ENGINE_TESTING)
bool decodeAmapTile(const uint8_t* data, size_t size,
                    std::vector<AmapDecodedLayerPart>& out,
                    std::string* error) {
    AmapClassicSourceBundle::Impl::DecodedTile tile;
    const bool ok = AmapClassicSourceBundle::Impl::decodeType1(
        data, size, tile, error);
    out = std::move(tile.parts);
    return ok;
}

bool decodeAmapPoiTile(const uint8_t* data, size_t size,
                       std::vector<AmapDecodedLayerPart>& out,
                       std::string* error) {
    AmapClassicSourceBundle::Impl::DecodedTile tile;
    const bool ok = AmapClassicSourceBundle::Impl::decodePoi(
        data, size, tile, error);
    out = std::move(tile.parts);
    return ok;
}
#endif

}  // namespace earth_engine
