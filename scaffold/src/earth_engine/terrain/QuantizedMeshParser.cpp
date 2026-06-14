#include "QuantizedMeshParser.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace earth_engine {
namespace {

constexpr size_t kHeaderSize = 92;  // 11×8 + 4 = 92 (3d center + 2f height + 7d bounds + 1u vc)

/// Barycentric point-in-triangle test with height interpolation.
bool pointInTriangle(double px, double py,
                     double ax, double ay, double bx, double by,
                     double cx, double cy,
                     double& outU, double& outV) {
    double v0x = cx - ax, v0y = cy - ay;
    double v1x = bx - ax, v1y = by - ay;
    double v2x = px - ax, v2y = py - ay;

    double dot00 = v0x * v0x + v0y * v0y;
    double dot01 = v0x * v1x + v0y * v1y;
    double dot02 = v0x * v2x + v0y * v2y;
    double dot11 = v1x * v1x + v1y * v1y;
    double dot12 = v1x * v2x + v1y * v2y;

    double inv = 1.0 / (dot00 * dot11 - dot01 * dot01);
    outU = (dot11 * dot02 - dot01 * dot12) * inv;
    outV = (dot00 * dot12 - dot01 * dot02) * inv;

    return (outU >= 0) && (outV >= 0) && (outU + outV <= 1.0);
}

} // namespace

std::unique_ptr<DecodedHeightmap> QuantizedMeshParser::parseAndRasterize(
    const uint8_t* data, size_t len, int outputGridSize) {

    if (len < 88 || !data) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "QMParser", "fail: len=%zu < 88", len);
#endif
        return nullptr;
    }

    // --- Parse header ---
    size_t offset = 0;
    Header hdr;
    auto readD = [&]() -> double {
        double v;
        std::memcpy(&v, data + offset, 8);
        offset += 8;
        return v;
    };
    auto readF = [&]() -> float {
        float v;
        std::memcpy(&v, data + offset, 4);
        offset += 4;
        return v;
    };
    auto readU32 = [&]() -> uint32_t {
        uint32_t v;
        std::memcpy(&v, data + offset, 4);
        offset += 4;
        return v;
    };

    hdr.centerX = readD();
    hdr.centerY = readD();
    hdr.centerZ = readD();
    hdr.minimumHeight = static_cast<double>(readF());
    hdr.maximumHeight = static_cast<double>(readF());
    hdr.boundingSphereX = readD();
    hdr.boundingSphereY = readD();
    hdr.boundingSphereZ = readD();
    hdr.boundingSphereRadius = readD();
    hdr.horizonOcclusionX = readD();
    hdr.horizonOcclusionY = readD();
    hdr.horizonOcclusionZ = readD();
    hdr.vertexCount = readU32();
    const uint32_t vertexCount = hdr.vertexCount;
    if (vertexCount == 0 || vertexCount > 500000) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "QMParser", "fail: vc=%u", vertexCount);
#endif
        return nullptr;
    }

    // Detect header padding: some tilers produce 96-byte headers.
    // Validate by checking if U-buffer bounds fit within file.
    {
        size_t stdEnd = offset + vertexCount * 6;  // U+V+H, 3×2 bytes each
        if (stdEnd + 8 > len && offset + 4 + vertexCount * 6 + 8 <= len) {
            // 92-byte header would overflow; 96-byte fits → skip padding
            offset += 4;
        }
    }

    // --- Parse U, V, Height buffers (zigzag delta encoded uint16_t arrays) ---
    auto readU16s = [&](size_t count) -> std::vector<uint16_t> {
        std::vector<uint16_t> out(count);
        if (offset + count * 2 > len) {
            out.clear();
            return out;
        }
        for (size_t i = 0; i < count; ++i) {
            uint16_t v;
            std::memcpy(&v, data + offset, 2);
            offset += 2;
            out[i] = v;
        }
        return out;
    };

    auto uBuf = readU16s(vertexCount);
    auto vBuf = readU16s(vertexCount);
    auto hBuf = readU16s(vertexCount);
    if (uBuf.empty() || vBuf.empty() || hBuf.empty()) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "QMParser", "fail: uBuf=%zu vBuf=%zu hBuf=%zu",
            uBuf.size(), vBuf.size(), hBuf.size());
#endif
        return nullptr;
    }

    // Decode zigzag delta to get U/V/Height ratios
    std::vector<double> uRatio(vertexCount), vRatio(vertexCount), heightRatio(vertexCount);
    int32_t uAcc = 0, vAcc = 0, hAcc = 0;
    for (uint32_t i = 0; i < vertexCount; ++i) {
        uAcc += zigZagDecode(static_cast<int32_t>(uBuf[i]));
        vAcc += zigZagDecode(static_cast<int32_t>(vBuf[i]));
        hAcc += zigZagDecode(static_cast<int32_t>(hBuf[i]));
        uRatio[i] = static_cast<double>(uAcc) / 32767.0;
        vRatio[i] = static_cast<double>(vAcc) / 32767.0;
        heightRatio[i] = static_cast<double>(hAcc) / 32767.0;
    }

    // --- Parse indices ---
    // Some tilers add 2-byte alignment padding even when vertexCount ≤ 65536.
    // Try unaligned first; if garbage, retry with alignment to 4-byte boundary. 
    uint32_t triangleCount = 0;
    bool use32BitIndices = (vertexCount > 65536);
    size_t idxOffset = offset;

    if (use32BitIndices && (offset % 4) != 0) offset += 2;
    if (offset + 4 > len) return nullptr;
    triangleCount = readU32();

    if (triangleCount == 0 || triangleCount > vertexCount * 4) {
        // Retry with 4-byte alignment (some tilers pad to 4-byte boundary)
        offset = idxOffset;
        if ((offset % 4) != 0) offset += 2;
        if (offset + 4 > len) return nullptr;
        triangleCount = readU32();
#ifdef __ANDROID__
        if (triangleCount > 0 && triangleCount <= vertexCount * 4)
            __android_log_print(ANDROID_LOG_INFO, "QMParser", "aligned triCount=%u", triangleCount);
#endif
    }

    const uint32_t indicesCount = triangleCount * 3;
    size_t indexSize = use32BitIndices ? 4 : 2;
    if (offset + indicesCount * indexSize > len || triangleCount == 0 || triangleCount > vertexCount * 4) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "QMParser", "fail: idx overflow off=%zu cnt=%u sz=%zu len=%zu",
            offset, indicesCount, indexSize, len);
#endif
        return nullptr;
    }

    // Decode zigzag-delta indices
    std::vector<uint32_t> indices(indicesCount);
    if (use32BitIndices) {
        uint32_t highest = 0;
        for (uint32_t i = 0; i < indicesCount; ++i) {
            uint32_t code;
            std::memcpy(&code, data + offset, 4);
            offset += 4;
            uint32_t decoded = highest - code;
            indices[i] = decoded;
            if (code == 0) ++highest;
        }
    } else {
        uint32_t highest = 0;
        for (uint32_t i = 0; i < indicesCount; ++i) {
            uint16_t code;
            std::memcpy(&code, data + offset, 2);
            offset += 2;
            uint32_t decoded = highest - code;
            indices[i] = decoded;
            if (code == 0) ++highest;
        }
    }

    // --- Rasterize triangles into a regular grid ---
    const double heightRange = hdr.maximumHeight - hdr.minimumHeight;
    const int n = outputGridSize + 1;  // vertices per side
    auto hm = std::make_unique<DecodedHeightmap>();
    hm->tileSize = n;
    hm->heights.resize(static_cast<size_t>(n * n), 0.0f);
    hm->minHeight = hdr.minimumHeight;
    hm->maxHeight = hdr.maximumHeight;
    hm->heightFactor = 1.0f;
    hm->noDataValues = {};

    // For each grid vertex, find the triangle that contains its (u,v) and
    // barycentrically interpolate the height.
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const double targetU = static_cast<double>(x) / static_cast<double>(outputGridSize);
            const double targetV = static_cast<double>(y) / static_cast<double>(outputGridSize);

            double bestH = hdr.minimumHeight;
            bool found = false;

            // Search all triangles for the one containing (targetU, targetV)
            for (uint32_t t = 0; t < triangleCount; ++t) {
                uint32_t i0 = indices[t * 3];
                uint32_t i1 = indices[t * 3 + 1];
                uint32_t i2 = indices[t * 3 + 2];

                if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;

                double w1, w2;
                if (pointInTriangle(targetU, targetV,
                                    uRatio[i0], vRatio[i0],
                                    uRatio[i1], vRatio[i1],
                                    uRatio[i2], vRatio[i2], w1, w2)) {
                    double w0 = 1.0 - w1 - w2;
                    bestH = hdr.minimumHeight + heightRange *
                        (w0 * heightRatio[i0] + w1 * heightRatio[i1] + w2 * heightRatio[i2]);
                    found = true;
                    break;
                }
            }

            if (!found) {
                // Point outside the mesh — find nearest vertex
                double bestDist = std::numeric_limits<double>::max();
                for (uint32_t i = 0; i < vertexCount; ++i) {
                    double du = uRatio[i] - targetU;
                    double dv = vRatio[i] - targetV;
                    double dist = du * du + dv * dv;
                    if (dist < bestDist) {
                        bestDist = dist;
                        bestH = hdr.minimumHeight + heightRange * heightRatio[i];
                    }
                }
            }

            size_t idx = static_cast<size_t>(y) * static_cast<size_t>(n) + static_cast<size_t>(x);
            hm->heights[idx] = static_cast<float>(bestH);
        }
    }

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "QMParser", "parseAndRasterize OK: %dx%d heights, %u verts, %u tris",
        n, n, vertexCount, triangleCount);
#endif
    return hm;
}

std::unique_ptr<SurfaceTileMesh> QuantizedMeshParser::parseToSurfaceTileMesh(
    const uint8_t* data, size_t len, const Rectangle& bounds) {

    if (len < kHeaderSize || !data) return nullptr;

    // --- Helper readers ---
    size_t offset = 0;
    auto readD = [&]() { double v; std::memcpy(&v, data + offset, 8); offset += 8; return v; };
    auto readF = [&]() { float v; std::memcpy(&v, data + offset, 4); offset += 4; return v; };
    auto readU32 = [&]() { uint32_t v; std::memcpy(&v, data + offset, 4); offset += 4; return v; };

    // --- Parse header ---
    Header hdr;
    hdr.centerX = readD(); hdr.centerY = readD(); hdr.centerZ = readD();
    hdr.minimumHeight = readF(); hdr.maximumHeight = readF();
    hdr.boundingSphereX = readD(); hdr.boundingSphereY = readD(); hdr.boundingSphereZ = readD();
    hdr.boundingSphereRadius = readD();
    hdr.horizonOcclusionX = readD(); hdr.horizonOcclusionY = readD(); hdr.horizonOcclusionZ = readD();
    hdr.vertexCount = readU32();

    const uint32_t vc = hdr.vertexCount;
    if (vc == 0 || vc > 500000) return nullptr;

    // --- Parse U/V/Height buffers ---
    auto readU16s = [&](size_t n) -> std::vector<uint16_t> {
        if (offset + n * 2 > len) return {};
        std::vector<uint16_t> out(n);
        for (size_t i = 0; i < n; ++i) { std::memcpy(&out[i], data + offset, 2); offset += 2; }
        return out;
    };
    auto uBuf = readU16s(vc), vBuf = readU16s(vc), hBuf = readU16s(vc);
    if (uBuf.empty()) return nullptr;

    // Decode zigzag delta
    int32_t uAcc = 0, vAcc = 0, hAcc = 0;
    for (uint32_t i = 0; i < vc; ++i) {
        uAcc += zigZagDecode(uBuf[i]);
        vAcc += zigZagDecode(vBuf[i]);
        hAcc += zigZagDecode(hBuf[i]);
        uBuf[i] = static_cast<uint16_t>(uAcc);
        vBuf[i] = static_cast<uint16_t>(vAcc);
        hBuf[i] = static_cast<uint16_t>(hAcc);
    }

    // --- Parse indices ---
    bool idx32 = (vc > 65536);
    if (idx32 && (offset % 4) != 0) offset += 2;
    uint32_t triCount = readU32();
    const uint32_t idxCount = triCount * 3;
    size_t idxByte = idx32 ? 4 : 2;
    if (offset + idxCount * idxByte > len) return nullptr;

    std::vector<uint32_t> indices(idxCount);
    uint32_t highest = 0;
    for (uint32_t i = 0; i < idxCount; ++i) {
        uint32_t code;
        if (idx32) { std::memcpy(&code, data + offset, 4); offset += 4; }
        else       { uint16_t c; std::memcpy(&c, data + offset, 2); offset += 2; code = c; }
        uint32_t dec = highest - code;
        indices[i] = dec;
        if (code == 0) ++highest;
    }

    // --- Parse edge indices ---
    auto readEdge = [&]() -> std::vector<uint32_t> {
        uint32_t ec = readU32();
        std::vector<uint32_t> out(ec);
        size_t eb = idx32 ? 4 : 2;
        for (uint32_t i = 0; i < ec; ++i) {
            uint32_t code;
            if (idx32) { std::memcpy(&code, data + offset, 4); offset += 4; }
            else       { uint16_t c; std::memcpy(&c, data + offset, 2); offset += 2; code = c; }
            out[i] = code;
        }
        return out;
    };
    auto west  = readEdge(), south = readEdge(), east = readEdge(), north = readEdge();

    auto mesh = std::make_unique<SurfaceTileMesh>();

    // --- Parse extensions (oct-encoded normals, water mask, metadata) ---
    WaterMask waterMask;
    std::vector<Vec3> octNormals;  // decoded from extension ID=1

    constexpr size_t kExtHeaderSize = 5;
    while (offset + kExtHeaderSize <= len) {
        uint8_t extId = data[offset]; offset += 1;
        uint32_t extLen; std::memcpy(&extLen, data + offset, 4); offset += 4;
        if (offset + extLen > len) break;

        if (extId == 1) {
            // Oct-encoded per-vertex normals (cesium-native attribute compression)
            size_t normCount = extLen / 2;
            octNormals.reserve(normCount);
            for (size_t i = 0; i < normCount; ++i) {
                uint8_t ox = data[offset + i * 2];
                uint8_t oy = data[offset + i * 2 + 1];
                // octDecode (cesium-native AttributeCompression)
                double fx = static_cast<double>(ox) * 2.0 / 255.0 - 1.0;
                double fy = static_cast<double>(oy) * 2.0 / 255.0 - 1.0;
                double fz = 1.0 - std::abs(fx) - std::abs(fy);
                if (fz < 0.0) {
                    double oldX = fx;
                    fx = (1.0 - std::abs(fy)) * (oldX >= 0.0 ? 1.0 : -1.0);
                    fy = (1.0 - std::abs(oldX)) * (fy >= 0.0 ? 1.0 : -1.0);
                }
                double lenInv = 1.0 / std::sqrt(fx * fx + fy * fy + fz * fz);
                octNormals.push_back(Vec3(fx * lenInv, fy * lenInv, fz * lenInv));
            }
        } else if (extId == 2 && extLen == 65536) {
            waterMask.allLand = false;
            waterMask.allWater = false;
            waterMask.data.resize(256 * 256 * 4);
            for (int i = 0; i < 256 * 256; ++i) {
                uint8_t v = data[offset + i];
                waterMask.data[i * 4] = 255;
                waterMask.data[i * 4 + 1] = 255;
                waterMask.data[i * 4 + 2] = 255;
                waterMask.data[i * 4 + 3] = v;
            }
        } else if (extId == 2 && extLen == 1) {
            uint8_t v = data[offset];
            waterMask.allWater = (v != 0);
            waterMask.allLand = !waterMask.allWater;
        } else if (extId == 4 && extLen > 0) {
            // cesium-native: metadata JSON with availability rectangles
            std::string metadataJson(reinterpret_cast<const char*>(data + offset), extLen);
            // Parse "available" array: [[{startX,startY,endX,endY},...],...]
            auto j = nlohmann::json::parse(metadataJson, nullptr, false);
            if (!j.is_discarded() && j.contains("available") && j["available"].is_array()) {
                for (const auto& levelRanges : j["available"]) {
                    if (!levelRanges.is_array()) continue;
                    for (const auto& range : levelRanges) {
                        if (!range.is_object()) continue;
                        mesh->metadataAvailability.push_back({
                            range.value("startX", 0),
                            range.value("startY", 0),
                            range.value("endX", 0),
                            range.value("endY", 0)
                        });
                    }
                }
            }
        }
        offset += extLen;
    }

    // --- Build ECEF vertices ---
    const auto& ellipsoid = Ellipsoid::WGS84();
    const double westLng  = bounds.west(), eastLng  = bounds.east();
    const double southLat = bounds.south(), northLat = bounds.north();
    const double hMin = hdr.minimumHeight, hRange = hdr.maximumHeight - hMin;
    const double uScale = 1.0 / 32767.0, hScale = 1.0 / 32767.0;

    mesh->gridSize = 0;  // irregular mesh
    mesh->winding = SurfaceTileMeshWinding::Outward;
    mesh->sampling = SurfaceTileSampling::WebMercatorVToWgs84Ecef;
    mesh->vertices.reserve(vc);
    mesh->indices.reserve(idxCount);

    // Vertices
    for (uint32_t i = 0; i < vc; ++i) {
        double u = static_cast<double>(uBuf[i]) * uScale;
        double v = static_cast<double>(vBuf[i]) * uScale;
        double h = hMin + static_cast<double>(hBuf[i]) * hScale * hRange;
        double lng = westLng + (eastLng - westLng) * std::clamp(u, 0.0, 1.0);
        double lat = southLat + (northLat - southLat) * std::clamp(v, 0.0, 1.0);

        SurfaceVertex sv;
        sv.positionEcef = ellipsoid.cartographicToCartesian(
            Cartographic::fromRadians(lng, lat, h));
        sv.normalEcef = Vec3::zero();  // computed below
        // Quantized-mesh v=0 is the south edge, while SurfaceTile imagery
        // UVs use v=0 at the north edge to match decoded raster rows.
        sv.uv = {
            static_cast<float>(u),
            static_cast<float>(1.0 - std::clamp(v, 0.0, 1.0))
        };
        mesh->vertices.push_back(sv);
    }

    // Indices
    for (uint32_t idx : indices) mesh->indices.push_back(idx);

    // --- Apply normals ---
    if (octNormals.size() == vc) {
        // Use the oct-encoded per-vertex normals (cesium-native quality)
        for (uint32_t i = 0; i < vc; ++i) {
            mesh->vertices[i].normalEcef = octNormals[i];
        }
    } else {
        // Fallback: face-average normals from the triangulation
        for (uint32_t t = 0; t < triCount; ++t) {
            uint32_t a = indices[t * 3], b = indices[t * 3 + 1], c = indices[t * 3 + 2];
            SurfaceVertex& va = mesh->vertices[a];
            SurfaceVertex& vb = mesh->vertices[b];
            SurfaceVertex& vc = mesh->vertices[c];
            Vec3 fn = (vb.positionEcef - va.positionEcef)
                .cross(vc.positionEcef - va.positionEcef);
            va.normalEcef += fn;
            vb.normalEcef += fn;
            vc.normalEcef += fn;
        }
        for (auto& v : mesh->vertices) {
            if (v.normalEcef.lengthSquared() > 0.0)
                v.normalEcef = v.normalEcef.normalized();
            else
                v.normalEcef = ellipsoid.geodeticSurfaceNormal(v.positionEcef);
        }
    }

    // --- Skirt ---
    mesh->skirtMeta.noSkirtVerticesBegin = 0;
    mesh->skirtMeta.noSkirtVerticesCount = vc;
    mesh->skirtMeta.noSkirtIndicesBegin = 0;
    mesh->skirtMeta.noSkirtIndicesCount = static_cast<uint32_t>(mesh->indices.size());

    // cesium-native skirt: partial_sort_copy each edge by UV coordinate,
    // then create bottom vertices and triangle strips.
    const double lonOff = (eastLng - westLng) * 0.0001;
    const double latOff = (northLat - southLat) * 0.0001;
    const double skirtH = 5.0 * (ellipsoid.semiMajorAxis() * 0.25 / 65.0) * bounds.width();

    auto addSkirtEdge = [&](const std::vector<uint32_t>& edgeIdx,
                             double lo, double la, bool reverse) {
        if (edgeIdx.size() < 2) return;
        const uint32_t firstSkirt = static_cast<uint32_t>(mesh->vertices.size());

        for (size_t i = 0; i < edgeIdx.size(); ++i) {
            size_t ei = reverse ? (edgeIdx.size() - 1 - i) : i;
            const SurfaceVertex& top = mesh->vertices[edgeIdx[ei]];
            Cartographic cart = ellipsoid.cartographicToCartesian(top.positionEcef);
            Cartographic sc = Cartographic::fromRadians(
                cart.longitude() + lo, cart.latitude() + la, cart.height() - skirtH);

            SurfaceVertex sv;
            sv.positionEcef = ellipsoid.cartographicToCartesian(sc);
            sv.normalEcef = ellipsoid.geodeticSurfaceNormal(sv.positionEcef);
            sv.uv = top.uv;
            mesh->vertices.push_back(sv);
        }

        for (size_t i = 0; i + 1 < edgeIdx.size(); ++i) {
            uint32_t t0 = edgeIdx[reverse ? (edgeIdx.size() - 1 - i) : i];
            uint32_t t1 = edgeIdx[reverse ? (edgeIdx.size() - 2 - i) : (i + 1)];
            uint32_t s0 = firstSkirt + static_cast<uint32_t>(i);
            uint32_t s1 = firstSkirt + static_cast<uint32_t>(i + 1);
            mesh->indices.push_back(t0); mesh->indices.push_back(t1); mesh->indices.push_back(s0);
            mesh->indices.push_back(s0); mesh->indices.push_back(t1); mesh->indices.push_back(s1);
        }
    };

    // cesium-native QuantizedMeshLoader::addSkirts: sort by UV before assembling.
    // West: sort by v asc (south→north)
    // South: sort by u desc (east→west)
    // East: sort by v desc (north→south)
    // North: sort by u asc (west→east)
    auto sortByU = [&](const std::vector<uint32_t>& idx, bool desc) {
        std::vector<uint32_t> s = idx;
        std::sort(s.begin(), s.end(), [&](uint32_t a, uint32_t b) {
            double ua = (uBuf.size() > a) ? (static_cast<double>(uBuf[a]) / 32767.0) : 0.0;
            double ub = (uBuf.size() > b) ? (static_cast<double>(uBuf[b]) / 32767.0) : 0.0;
            return desc ? (ua > ub) : (ua < ub);
        });
        return s;
    };
    auto sortByV = [&](const std::vector<uint32_t>& idx, bool desc) {
        std::vector<uint32_t> s = idx;
        std::sort(s.begin(), s.end(), [&](uint32_t a, uint32_t b) {
            double va = (vBuf.size() > a) ? (static_cast<double>(vBuf[a]) / 32767.0) : 0.0;
            double vb = (vBuf.size() > b) ? (static_cast<double>(vBuf[b]) / 32767.0) : 0.0;
            return desc ? (va > vb) : (va < vb);
        });
        return s;
    };

    addSkirtEdge(sortByV(west, false), -lonOff, 0, false);
    addSkirtEdge(sortByU(south, true), 0, -latOff, false);
    addSkirtEdge(sortByV(east, true), lonOff, 0, false);
    addSkirtEdge(sortByU(north, false), 0, latOff, false);

    mesh->waterMask = std::move(waterMask);
    return mesh;
}

} // namespace earth_engine
