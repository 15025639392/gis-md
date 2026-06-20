#include "QuantizedMeshParser.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
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

int jsonUint32OrDefault(const nlohmann::json& object, const char* name) {
    auto it = object.find(name);
    if (it == object.end()) return 0;

    uint64_t value = 0;
    if (it->is_number_unsigned()) {
        value = it->get<uint64_t>();
    } else if (it->is_number_integer()) {
        const int64_t signedValue = it->get<int64_t>();
        if (signedValue < 0) return 0;
        value = static_cast<uint64_t>(signedValue);
    } else {
        return 0;
    }

    constexpr uint64_t kMaxUint32 = 0xffffffffull;
    if (value > kMaxUint32) return 0;
    return value > static_cast<uint64_t>(std::numeric_limits<int>::max())
        ? std::numeric_limits<int>::max()
        : static_cast<int>(value);
}

std::vector<std::array<int, 5>> parseMetadataAvailabilityJson(
    const std::string& metadataJson) {
    std::vector<std::array<int, 5>> availability;
    auto j = nlohmann::json::parse(metadataJson, nullptr, false);
    if (j.is_discarded() ||
        !j.contains("available") ||
        !j["available"].is_array()) {
        return availability;
    }

    int subArrayIndex = 0;
    for (const auto& levelRanges : j["available"]) {
        if (!levelRanges.is_array()) {
            continue;
        }
        for (const auto& range : levelRanges) {
            if (!range.is_object()) continue;
            availability.push_back({
                subArrayIndex,
                jsonUint32OrDefault(range, "startX"),
                jsonUint32OrDefault(range, "startY"),
                jsonUint32OrDefault(range, "endX"),
                jsonUint32OrDefault(range, "endY")
            });
        }
        ++subArrayIndex;
    }

    return availability;
}

double calcQuadtreeMaxGeometricError(const Ellipsoid& ellipsoid) {
    constexpr double kTerrainHeightmapQuality = 0.25;
    constexpr double kHeightmapWidth = 65.0;
    return ellipsoid.maximumRadius() *
           kTerrainHeightmapQuality /
           kHeightmapWidth;
}

double calculateSkirtHeight(const Ellipsoid& ellipsoid,
                            const Rectangle& rectangle) {
    return calcQuadtreeMaxGeometricError(ellipsoid) *
           rectangle.width() *
           5.0;
}

} // namespace

std::unique_ptr<DecodedHeightmap> QuantizedMeshParser::parseAndRasterize(
    const uint8_t* data, size_t len, int outputGridSize) {

    if (len < kHeaderSize || !data) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "QMParser", "fail: len=%zu < %zu", len, kHeaderSize);
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
    if (std::any_of(indices.begin(), indices.end(), [&](uint32_t idx) {
            return idx >= vertexCount;
        })) {
#ifdef __ANDROID__
        __android_log_print(
            ANDROID_LOG_ERROR,
            "QMParser",
            "fail: decoded raster index outside vertex range vc=%u tri=%u",
            vertexCount,
            triangleCount);
#endif
        return nullptr;
    }

    auto validateEdge = [&]() -> bool {
        if (offset + sizeof(uint32_t) > len) {
            return false;
        }
        const uint32_t edgeCount = readU32();
        if (static_cast<size_t>(edgeCount) > (len - offset) / indexSize) {
            return false;
        }
        for (uint32_t i = 0; i < edgeCount; ++i) {
            uint32_t edgeIndex = 0;
            if (use32BitIndices) {
                std::memcpy(&edgeIndex, data + offset, sizeof(uint32_t));
                offset += sizeof(uint32_t);
            } else {
                uint16_t value = 0;
                std::memcpy(&value, data + offset, sizeof(uint16_t));
                offset += sizeof(uint16_t);
                edgeIndex = value;
            }
            if (edgeIndex >= vertexCount) {
                return false;
            }
        }
        return true;
    };
    if (!validateEdge() || !validateEdge() ||
        !validateEdge() || !validateEdge()) {
#ifdef __ANDROID__
        __android_log_print(
            ANDROID_LOG_ERROR,
            "QMParser",
            "fail: raster edge buffer invalid vc=%u tri=%u off=%zu len=%zu",
            vertexCount,
            triangleCount,
            offset,
            len);
#endif
        return nullptr;
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
    auto edgeStats = [&](int rowStart, int rowStep, int count) {
        int valid = 0;
        float minH = std::numeric_limits<float>::max();
        float maxH = std::numeric_limits<float>::lowest();
        for (int i = 0; i < count; ++i) {
            float h = hm->heights[rowStart + i * rowStep];
            if (hm->isNoData(h)) continue;
            ++valid;
            minH = std::min(minH, h);
            maxH = std::max(maxH, h);
        }
        if (valid == 0) {
            minH = maxH = 0.0f;
        }
        return std::tuple<int, float, float>(valid, minH, maxH);
    };
    const auto [northValid, northMin, northMax] = edgeStats(0, 1, n);
    const auto [southValid, southMin, southMax] = edgeStats((n - 1) * n, 1, n);
    const auto [westValid, westMin, westMax] = edgeStats(0, n, n);
    const auto [eastValid, eastMin, eastMax] = edgeStats(n - 1, n, n);
    __android_log_print(
        ANDROID_LOG_INFO,
        "QMParser",
        "parseAndRasterize OK: %dx%d heights, %u verts, %u tris edge N=%d[%.2f..%.2f] S=%d[%.2f..%.2f] W=%d[%.2f..%.2f] E=%d[%.2f..%.2f]",
        n, n, vertexCount, triangleCount,
        northValid, northMin, northMax,
        southValid, southMin, southMax,
        westValid, westMin, westMax,
        eastValid, eastMin, eastMax);
#endif
    return hm;
}

std::vector<std::array<int, 5>> QuantizedMeshParser::parseMetadataAvailability(
    const uint8_t* data, size_t len) {
    if (len < kHeaderSize || !data) return {};

    uint32_t vertexCount = 0;
    std::memcpy(&vertexCount, data + 88, sizeof(uint32_t));
    if (vertexCount == 0 || vertexCount > 500000) return {};

    auto parseFromPayloadOffset =
        [&](size_t payloadOffset) -> std::optional<std::vector<std::array<int, 5>>> {
        size_t offset = payloadOffset;
        const size_t vertexAttributeBytes =
            static_cast<size_t>(vertexCount) * 3u * sizeof(uint16_t);
        if (offset + vertexAttributeBytes > len) return std::nullopt;
        offset += vertexAttributeBytes;

        const bool idx32 = vertexCount > 65536;
        const size_t indexSize = idx32 ? sizeof(uint32_t) : sizeof(uint16_t);
        if (idx32 && (offset % 4) != 0) offset += 2;
        if (offset + sizeof(uint32_t) > len) return std::nullopt;

        uint32_t triangleCount = 0;
        std::memcpy(&triangleCount, data + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        const size_t indicesCount = static_cast<size_t>(triangleCount) * 3u;
        if (indicesCount > (len - offset) / indexSize) return std::nullopt;
        offset += indicesCount * indexSize;

        for (int edge = 0; edge < 4; ++edge) {
            if (offset + sizeof(uint32_t) > len) return std::nullopt;
            uint32_t edgeCount = 0;
            std::memcpy(&edgeCount, data + offset, sizeof(uint32_t));
            offset += sizeof(uint32_t);
            if (static_cast<size_t>(edgeCount) > (len - offset) / indexSize) {
                return std::nullopt;
            }
            offset += static_cast<size_t>(edgeCount) * indexSize;
        }

        constexpr size_t kExtHeaderSize = 5;
        std::optional<std::vector<std::array<int, 5>>> latestMetadata;
        while (offset + kExtHeaderSize <= len) {
            const uint8_t extId = data[offset];
            offset += sizeof(uint8_t);

            uint32_t extLen = 0;
            std::memcpy(&extLen, data + offset, sizeof(uint32_t));
            offset += sizeof(uint32_t);

            if (extId == 1 &&
                offset + static_cast<size_t>(vertexCount) * 2u > len) {
                break;
            }
            if (extId == 4) {
                if (offset + sizeof(uint32_t) > len) break;
                uint32_t metadataJsonLength = 0;
                std::memcpy(&metadataJsonLength, data + offset, sizeof(uint32_t));
                const size_t jsonOffset = offset + sizeof(uint32_t);
                if (metadataJsonLength > len - jsonOffset) break;
                latestMetadata = parseMetadataAvailabilityJson(std::string(
                    reinterpret_cast<const char*>(data + jsonOffset),
                    metadataJsonLength));
            }

            if (extLen > len - offset) break;
            offset += extLen;
        }

        return latestMetadata;
    };

    std::optional<std::vector<std::array<int, 5>>> availability =
        parseFromPayloadOffset(kHeaderSize);
    if (availability) return *availability;

    availability = parseFromPayloadOffset(kHeaderSize + sizeof(uint32_t));
    return availability.value_or(std::vector<std::array<int, 5>>{});
}

std::unique_ptr<SurfaceTileMesh> QuantizedMeshParser::parseToSurfaceTileMesh(
    const uint8_t* data,
    size_t len,
    const Rectangle& bounds,
    bool enableWaterMask) {

    if (len < kHeaderSize || !data) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "QMParser",
            "parseToSurfaceTileMesh fail: invalid input len=%zu data=%p",
            len, static_cast<const void*>(data));
#endif
        return nullptr;
    }

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
    if (vc == 0 || vc > 500000) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "QMParser",
            "parseToSurfaceTileMesh fail: vertexCount=%u len=%zu off=%zu",
            vc, len, offset);
#endif
        return nullptr;
    }

    // --- Parse U/V/Height buffers ---
    auto readU16s = [&](size_t n) -> std::vector<uint16_t> {
        if (offset + n * 2 > len) return {};
        std::vector<uint16_t> out(n);
        for (size_t i = 0; i < n; ++i) { std::memcpy(&out[i], data + offset, 2); offset += 2; }
        return out;
    };
    auto uBuf = readU16s(vc), vBuf = readU16s(vc), hBuf = readU16s(vc);
    if (uBuf.empty() || vBuf.empty() || hBuf.empty()) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "QMParser",
            "parseToSurfaceTileMesh fail: u/v/h buffer truncated vc=%u off=%zu len=%zu u=%zu v=%zu h=%zu",
            vc, offset, len, uBuf.size(), vBuf.size(), hBuf.size());
#endif
        return nullptr;
    }

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
    const size_t idxOffset = offset;
    if (idx32 && (offset % 4) != 0) offset += 2;
    if (offset + 4 > len) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "QMParser",
            "parseToSurfaceTileMesh fail: missing triCount vc=%u off=%zu len=%zu",
            vc, offset, len);
#endif
        return nullptr;
    }
    uint32_t triCount = readU32();
    if (triCount == 0 || triCount > vc * 4) {
        // Retry with 4-byte alignment like parseAndRasterize.
        offset = idxOffset;
        if ((offset % 4) != 0) offset += 2;
        if (offset + 4 > len) {
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_ERROR, "QMParser",
                "parseToSurfaceTileMesh fail: missing aligned triCount vc=%u off=%zu len=%zu",
                vc, offset, len);
#endif
            return nullptr;
        }
        triCount = readU32();
#ifdef __ANDROID__
        if (triCount > 0 && triCount <= vc * 4) {
            __android_log_print(ANDROID_LOG_INFO, "QMParser",
                "aligned triCount=%u", triCount);
        }
#endif
    }
    const uint32_t idxCount = triCount * 3;
    size_t idxByte = idx32 ? 4 : 2;
    if (offset + idxCount * idxByte > len || triCount == 0 || triCount > vc * 4) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "QMParser",
            "parseToSurfaceTileMesh fail: index overflow vc=%u tri=%u idxCount=%u idxByte=%zu off=%zu len=%zu",
            vc, triCount, idxCount, idxByte, offset, len);
#endif
        return nullptr;
    }

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
    if (std::any_of(indices.begin(), indices.end(), [&](uint32_t idx) { return idx >= vc; })) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "QMParser",
            "parseToSurfaceTileMesh fail: decoded index outside vertex range vc=%u tri=%u",
            vc, triCount);
#endif
        return nullptr;
    }

    if (idxCount == 0 || triCount == 0) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "QMParser",
            "parseToSurfaceTileMesh fail: zero indices vc=%u tri=%u off=%zu len=%zu",
            vc, triCount, offset, len);
#endif
        return nullptr;
    }

    // --- Parse edge indices ---
    auto readEdge = [&]() -> std::optional<std::vector<uint32_t>> {
        if (offset + sizeof(uint32_t) > len) {
            return std::nullopt;
        }
        uint32_t ec = readU32();
        if (static_cast<size_t>(ec) > (len - offset) / idxByte) {
            return std::nullopt;
        }
        std::vector<uint32_t> out(ec);
        for (uint32_t i = 0; i < ec; ++i) {
            uint32_t code;
            if (idx32) { std::memcpy(&code, data + offset, 4); offset += 4; }
            else       { uint16_t c; std::memcpy(&c, data + offset, 2); offset += 2; code = c; }
            out[i] = code;
        }
        return out;
    };
    auto westEdge = readEdge();
    auto southEdge = readEdge();
    auto eastEdge = readEdge();
    auto northEdge = readEdge();

    if (!westEdge || !southEdge || !eastEdge || !northEdge) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "QMParser",
            "parseToSurfaceTileMesh fail: edge read overflow vc=%u tri=%u off=%zu len=%zu west=%zu south=%zu east=%zu north=%zu",
            vc, triCount, offset, len,
            westEdge ? westEdge->size() : 0,
            southEdge ? southEdge->size() : 0,
            eastEdge ? eastEdge->size() : 0,
            northEdge ? northEdge->size() : 0);
#endif
        return nullptr;
    }
    const std::vector<uint32_t>& west = *westEdge;
    const std::vector<uint32_t>& south = *southEdge;
    const std::vector<uint32_t>& east = *eastEdge;
    const std::vector<uint32_t>& north = *northEdge;
    auto edgeHasInvalidIndex = [&](const std::vector<uint32_t>& edge) {
        return std::any_of(edge.begin(), edge.end(), [&](uint32_t idx) { return idx >= vc; });
    };
    if (edgeHasInvalidIndex(west) || edgeHasInvalidIndex(south) ||
        edgeHasInvalidIndex(east) || edgeHasInvalidIndex(north)) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "QMParser",
            "parseToSurfaceTileMesh fail: edge index outside vertex range vc=%u",
            vc);
#endif
        return nullptr;
    }

    auto mesh = std::make_unique<SurfaceTileMesh>();
    mesh->rasterOverlayDetails.setGeographicRectangle(bounds);
    mesh->hasLocalOriginEcef = true;
    mesh->localOriginEcef =
        Vec3(hdr.boundingSphereX, hdr.boundingSphereY, hdr.boundingSphereZ);
    mesh->hasHeightRange = true;
    mesh->minimumHeight = hdr.minimumHeight;
    mesh->maximumHeight = hdr.maximumHeight;
    mesh->horizonOcclusionPoint =
        Vec3(hdr.horizonOcclusionX, hdr.horizonOcclusionY, hdr.horizonOcclusionZ);
    mesh->hasHorizonOcclusionPoint =
        mesh->horizonOcclusionPoint.lengthSquared() > 0.0;

    // --- Parse extensions (oct-encoded normals, water mask, metadata) ---
    WaterMask waterMask;
    std::vector<Vec3> octNormals;  // decoded from extension ID=1

    constexpr size_t kExtHeaderSize = 5;
    while (offset + kExtHeaderSize <= len) {
        uint8_t extId = data[offset]; offset += 1;
        uint32_t extLen; std::memcpy(&extLen, data + offset, 4); offset += 4;

        if (extId == 1) {
            // Oct-encoded per-vertex normals (cesium-native attribute compression)
            const size_t normCount = vc;
            if (offset + normCount * 2 > len) {
                break;
            }
            octNormals.clear();
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
        } else if (enableWaterMask && extId == 2 && extLen == 65536) {
            if (extLen > len - offset) break;
            waterMask.allLand = false;
            waterMask.allWater = false;
            waterMask.data.assign(256 * 256 * 4, 0);
            for (int i = 0; i < 256 * 256; ++i) {
                uint8_t v = data[offset + i];
                waterMask.data[i * 4] = 255;
                waterMask.data[i * 4 + 1] = 255;
                waterMask.data[i * 4 + 2] = 255;
                waterMask.data[i * 4 + 3] = v;
            }
        } else if (enableWaterMask && extId == 2 && extLen == 1) {
            if (extLen > len - offset) break;
            uint8_t v = data[offset];
            waterMask.allWater = (v != 0);
            waterMask.allLand = !waterMask.allWater;
            waterMask.data.clear();
        } else if (extId == 4) {
            // cesium-native: metadata JSON with availability rectangles.
            // Extension payload starts with uint32 metadataJsonLength, then JSON.
            // Format: "available": [[{startX,startY,endX,endY},...],...]
            // Each valid level array advances the availability level.
            // Aligned with cesium-native loadAvailabilityRectangles.
            if (offset + sizeof(uint32_t) > len) break;
            uint32_t metadataJsonLength = 0;
            std::memcpy(&metadataJsonLength, data + offset, sizeof(uint32_t));
            const size_t jsonOffset = offset + sizeof(uint32_t);
            if (metadataJsonLength > len - jsonOffset) break;
            std::string metadataJson(
                reinterpret_cast<const char*>(data + jsonOffset),
                metadataJsonLength);
            mesh->metadataAvailability =
                parseMetadataAvailabilityJson(metadataJson);
        }
        if (extLen > len - offset) break;
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
    const double skirtH = calculateSkirtHeight(ellipsoid, bounds);

    auto addSkirtEdge = [&](const std::vector<uint32_t>& edgeIdx,
                             double lo, double la, bool reverse) {
        if (edgeIdx.size() < 2) return;
        const uint32_t firstSkirt = static_cast<uint32_t>(mesh->vertices.size());

        for (size_t i = 0; i < edgeIdx.size(); ++i) {
            size_t ei = reverse ? (edgeIdx.size() - 1 - i) : i;
            const SurfaceVertex& top = mesh->vertices[edgeIdx[ei]];
            Cartographic cart = ellipsoid.cartesianToCartographic(top.positionEcef);
            Cartographic sc = Cartographic::fromRadians(
                cart.longitude() + lo, cart.latitude() + la, cart.height() - skirtH);

            SurfaceVertex sv;
            sv.positionEcef = ellipsoid.cartographicToCartesian(sc);
            sv.normalEcef = top.normalEcef;
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

    // cesium-native QuantizedMeshLoader::addSkirts:
    // Use std::partial_sort_copy into a pre-allocated buffer, matching
    // the cesium-native pattern exactly.
    // West edge: partial_sort_copy by v ascending (south→north)
    // South edge: partial_sort_copy by u descending (east→west)
    // East edge: partial_sort_copy by v descending (north→south)
    // North edge: partial_sort_copy by u ascending (west→east)
    uint32_t maxEdgeVertexCount = std::max({
        static_cast<uint32_t>(west.size()),
        static_cast<uint32_t>(south.size()),
        static_cast<uint32_t>(east.size()),
        static_cast<uint32_t>(north.size())});
    std::vector<uint32_t> sortEdgeIndices(maxEdgeVertexCount);

    auto partialSortEdge = [&](const std::vector<uint32_t>& edge,
                                auto comparator) -> std::vector<uint32_t> {
        if (edge.empty()) return {};
        uint32_t count = static_cast<uint32_t>(edge.size());
        std::partial_sort_copy(
            edge.begin(), edge.end(),
            sortEdgeIndices.begin(),
            sortEdgeIndices.begin() + count,
            comparator);
        return std::vector<uint32_t>(
            sortEdgeIndices.begin(),
            sortEdgeIndices.begin() + count);
    };

    // West: sort by v asc (uBuf/vBuf are 16-bit quantized, [0,32767])
    auto sortedWest = partialSortEdge(west,
        [&](uint32_t a, uint32_t b) {
            return vBuf[a] < vBuf[b];
        });
    // South: sort by u desc
    auto sortedSouth = partialSortEdge(south,
        [&](uint32_t a, uint32_t b) {
            return uBuf[a] > uBuf[b];
        });
    // East: sort by v desc
    auto sortedEast = partialSortEdge(east,
        [&](uint32_t a, uint32_t b) {
            return vBuf[a] > vBuf[b];
        });
    // North: sort by u asc
    auto sortedNorth = partialSortEdge(north,
        [&](uint32_t a, uint32_t b) {
            return uBuf[a] < uBuf[b];
        });

#ifdef __ANDROID__
    auto edgeUvSummary = [&](const char* name, const std::vector<uint32_t>& edge) {
        if (edge.empty()) {
            __android_log_print(
                ANDROID_LOG_INFO,
                "QMParser",
                "edge %s count=0",
                name);
            return;
        }
        const uint32_t first = edge.front();
        const uint32_t last = edge.back();
        __android_log_print(
            ANDROID_LOG_INFO,
            "QMParser",
            "edge %s count=%zu first=%u uv=%u/%u last=%u uv=%u/%u",
            name,
            edge.size(),
            first,
            static_cast<unsigned>(uBuf[first]),
            static_cast<unsigned>(vBuf[first]),
            last,
            static_cast<unsigned>(uBuf[last]),
            static_cast<unsigned>(vBuf[last]));
    };
    __android_log_print(
        ANDROID_LOG_INFO,
        "QMParser",
        "skirt summary vc=%u tri=%u west=%zu south=%zu east=%zu north=%zu skirtH=%.3f lonOff=%.7f latOff=%.7f",
        vc,
        triCount,
        west.size(),
        south.size(),
        east.size(),
        north.size(),
        skirtH,
        lonOff,
        latOff);
    edgeUvSummary("west", sortedWest);
    edgeUvSummary("south", sortedSouth);
    edgeUvSummary("east", sortedEast);
    edgeUvSummary("north", sortedNorth);
#endif

    addSkirtEdge(sortedWest,  -lonOff, 0,       false);
    addSkirtEdge(sortedSouth, 0,       -latOff, false);
    addSkirtEdge(sortedEast,  -lonOff, 0,       false);
    addSkirtEdge(sortedNorth, 0,       latOff,  false);

    mesh->gpuVertices.resize(mesh->vertices.size());
    const Vec3 localOrigin = mesh->localOriginEcef;
    for (size_t i = 0; i < mesh->vertices.size(); ++i) {
        const SurfaceVertex& src = mesh->vertices[i];
        SurfaceGpuVertex& dst = mesh->gpuVertices[i];
        const Vec3 rel = src.positionEcef - localOrigin;
        Vec3 nrm = src.normalEcef;
        if (nrm.lengthSquared() > 0.0) {
            nrm = nrm.normalized();
        } else {
            nrm = ellipsoid.geodeticSurfaceNormal(src.positionEcef);
        }
        dst.pos[0] = static_cast<float>(rel.x());
        dst.pos[1] = static_cast<float>(rel.y());
        dst.pos[2] = static_cast<float>(rel.z());
        dst.nrm[0] = static_cast<float>(nrm.x());
        dst.nrm[1] = static_cast<float>(nrm.y());
        dst.nrm[2] = static_cast<float>(nrm.z());
        dst.uv[0] = src.uv[0];
        dst.uv[1] = src.uv[1];
    }

    mesh->waterMask = std::move(waterMask);
    return mesh;
}

} // namespace earth_engine
