#include "QuantizedMeshParser.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include "../core/math/AttributeCompression.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/QuadtreeGeometricError.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <utility>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace earth_engine {
namespace {

constexpr size_t kHeaderSize = 92;  // 11×8 + 4 = 92 (3d center + 2f height + 7d bounds + 1u vc)

uint32_t jsonUint32OrDefault(const nlohmann::json& object, const char* name) {
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
    return static_cast<uint32_t>(value);
}

QuantizedMeshParser::MetadataAvailabilityParseResult parseMetadataAvailabilityJson(
    const std::string& metadataJson) {
    QuantizedMeshParser::MetadataAvailabilityParseResult result;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(metadataJson);
    } catch (const nlohmann::json::parse_error& e) {
        result.diagnostics.push_back(
            "Error when parsing metadata at byte offset " +
            std::to_string(e.byte) + ": " + e.what());
        return result;
    }
    if (!j.contains("available") || !j["available"].is_array()) {
        return result;
    }

    int subArrayIndex = 0;
    for (const auto& levelRanges : j["available"]) {
        if (!levelRanges.is_array()) {
            continue;
        }

        for (const auto& range : levelRanges) {
            if (!range.is_object()) continue;
            result.availability.push_back({
                static_cast<uint32_t>(subArrayIndex),
                jsonUint32OrDefault(range, "startX"),
                jsonUint32OrDefault(range, "startY"),
                jsonUint32OrDefault(range, "endX"),
                jsonUint32OrDefault(range, "endY")
            });
        }
        ++subArrayIndex;
    }

    return result;
}

double calculateSkirtHeight(const Ellipsoid& ellipsoid,
                            const Rectangle& rectangle) {
    return calcQuadtreeSkirtHeight(ellipsoid, rectangle);
}

std::string metadataExtensionTruncatedDiagnostic(uint32_t metadataJsonLength,
                                                 size_t availableBytes) {
    return "Quantized Mesh metadata extension is truncated: declared JSON length " +
           std::to_string(metadataJsonLength) + " exceeds available payload bytes " +
           std::to_string(availableBytes);
}

} // namespace

std::vector<QuantizedMeshAvailabilityRange> QuantizedMeshParser::parseMetadataAvailability(
    const uint8_t* data, size_t len) {
    return parseMetadataAvailabilityWithDiagnostics(data, len).availability;
}

QuantizedMeshParser::MetadataAvailabilityParseResult
QuantizedMeshParser::parseMetadataAvailabilityWithDiagnostics(
    const uint8_t* data, size_t len) {
    if (len < kHeaderSize || !data) return {};

    uint32_t vertexCount = 0;
    std::memcpy(&vertexCount, data + 88, sizeof(uint32_t));
    if (vertexCount == 0 || vertexCount > 500000) return {};

    auto parseFromPayloadOffset =
        [&](size_t payloadOffset) -> std::optional<MetadataAvailabilityParseResult> {
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
        std::optional<MetadataAvailabilityParseResult> latestMetadata;
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
                if (offset + sizeof(uint32_t) > len) {
                    latestMetadata = MetadataAvailabilityParseResult{};
                    latestMetadata->diagnostics.push_back(
                        metadataExtensionTruncatedDiagnostic(
                            0,
                            len - offset));
                    break;
                }
                uint32_t metadataJsonLength = 0;
                std::memcpy(&metadataJsonLength, data + offset, sizeof(uint32_t));
                const size_t jsonOffset = offset + sizeof(uint32_t);
                if (metadataJsonLength > len - jsonOffset) {
                    latestMetadata = MetadataAvailabilityParseResult{};
                    latestMetadata->diagnostics.push_back(
                        metadataExtensionTruncatedDiagnostic(
                            metadataJsonLength,
                            len - jsonOffset));
                    break;
                }
                latestMetadata = parseMetadataAvailabilityJson(std::string(
                    reinterpret_cast<const char*>(data + jsonOffset),
                    metadataJsonLength));
            }

            if (extLen > len - offset) break;
            offset += extLen;
        }

        return latestMetadata;
    };

    std::optional<MetadataAvailabilityParseResult> availability =
        parseFromPayloadOffset(kHeaderSize);
    if (availability) return *availability;

    availability = parseFromPayloadOffset(kHeaderSize + sizeof(uint32_t));
    return availability.value_or(MetadataAvailabilityParseResult{});
}

std::unique_ptr<QuantizedMeshParser::DecodedTile> QuantizedMeshParser::parseToDecodedTile(
    const uint8_t* data,
    size_t len,
    const Rectangle& bounds,
    bool enableWaterMask) {

    if (len < kHeaderSize || !data) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "QMParser",
            "parseToDecodedTile fail: invalid input len=%zu data=%p",
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
            "parseToDecodedTile fail: vertexCount=%u len=%zu off=%zu",
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
            "parseToDecodedTile fail: u/v/h buffer truncated vc=%u off=%zu len=%zu u=%zu v=%zu h=%zu",
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
    // 32-bit indices use 4-byte alignment. When vertexCount > 65536
    // and offset is not 4-byte aligned, insert 2-byte padding before
    // triCount. Matches cesium-native QuantizedMeshLoader.cpp.
    // NOTE: 16-bit indices NEVER have padding — skip only for idx32.
    if (idx32 && (offset % 4) != 0) {
        offset += 2;  // Skip intentional zero padding for 4-byte alignment
    }
    if (offset + 4 > len) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "QMParser",
            "parseToDecodedTile fail: missing triCount vc=%u off=%zu len=%zu",
            vc, offset, len);
#endif
        return nullptr;
    }
    uint32_t triCount = readU32();
    const size_t idxCount = static_cast<size_t>(triCount) * 3u;
    size_t idxByte = idx32 ? 4 : 2;
    if (idxCount > (len - offset) / idxByte) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "QMParser",
            "parseToDecodedTile fail: index overflow vc=%u tri=%u idxCount=%zu idxByte=%zu off=%zu len=%zu",
            vc, triCount, idxCount, idxByte, offset, len);
#endif
        return nullptr;
    }

    std::vector<uint32_t> indices(idxCount);
    uint32_t highest = 0;
    for (size_t i = 0; i < idxCount; ++i) {
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
            "parseToDecodedTile fail: decoded index outside vertex range vc=%u tri=%u",
            vc, triCount);
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
            "parseToDecodedTile fail: edge read overflow vc=%u tri=%u off=%zu len=%zu west=%zu south=%zu east=%zu north=%zu",
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
            "parseToDecodedTile fail: edge index outside vertex range vc=%u",
            vc);
#endif
        return nullptr;
    }

    auto decoded = std::make_unique<DecodedTile>();
    decoded->localOriginEcef =
        Vec3(hdr.boundingSphereX, hdr.boundingSphereY, hdr.boundingSphereZ);
    decoded->minimumHeight = hdr.minimumHeight;
    decoded->maximumHeight = hdr.maximumHeight;
    decoded->horizonOcclusionPoint =
        Vec3(hdr.horizonOcclusionX, hdr.horizonOcclusionY, hdr.horizonOcclusionZ);

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
                octNormals.push_back(
                    Vec3(AttributeCompression::octDecode(ox, oy)));
            }
        } else if (enableWaterMask && extId == 2 && extLen == 65536) {
            if (extLen > len - offset) break;
            waterMask.allLand = false;
            waterMask.allWater = false;
            waterMask.data.assign(data + offset, data + offset + extLen);
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
            if (offset + sizeof(uint32_t) > len) {
                decoded->diagnostics.push_back(
                    metadataExtensionTruncatedDiagnostic(
                        0,
                        len - offset));
                break;
            }
            uint32_t metadataJsonLength = 0;
            std::memcpy(&metadataJsonLength, data + offset, sizeof(uint32_t));
            const size_t jsonOffset = offset + sizeof(uint32_t);
            if (metadataJsonLength > len - jsonOffset) {
                decoded->diagnostics.push_back(
                    metadataExtensionTruncatedDiagnostic(
                        metadataJsonLength,
                        len - jsonOffset));
                break;
            }
            std::string metadataJson(
                reinterpret_cast<const char*>(data + jsonOffset),
                metadataJsonLength);
            decoded->hasMetadataAvailability = true;
            MetadataAvailabilityParseResult metadata =
                parseMetadataAvailabilityJson(metadataJson);
            decoded->metadataAvailability = std::move(metadata.availability);
            decoded->diagnostics.insert(
                decoded->diagnostics.end(),
                metadata.diagnostics.begin(),
                metadata.diagnostics.end());
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

    decoded->vertices.reserve(vc);
    decoded->indices.reserve(idxCount);

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
        decoded->vertices.push_back(sv);
    }

    // Indices
    for (uint32_t idx : indices) decoded->indices.push_back(idx);

    // --- Apply normals ---
    if (octNormals.size() == vc) {
        // Use the oct-encoded per-vertex normals (cesium-native quality)
        for (uint32_t i = 0; i < vc; ++i) {
            decoded->vertices[i].normalEcef = octNormals[i];
        }
    } else {
        // Fallback: face-average normals from the triangulation
        for (uint32_t t = 0; t < triCount; ++t) {
            uint32_t a = indices[t * 3], b = indices[t * 3 + 1], c = indices[t * 3 + 2];
            SurfaceVertex& va = decoded->vertices[a];
            SurfaceVertex& vb = decoded->vertices[b];
            SurfaceVertex& vc = decoded->vertices[c];
            Vec3 fn = (vb.positionEcef - va.positionEcef)
                .cross(vc.positionEcef - va.positionEcef);
            va.normalEcef += fn;
            vb.normalEcef += fn;
            vc.normalEcef += fn;
        }
        for (auto& v : decoded->vertices) {
            if (v.normalEcef.lengthSquared() > 0.0)
                v.normalEcef = v.normalEcef.normalized();
            else
                v.normalEcef = ellipsoid.geodeticSurfaceNormal(v.positionEcef);
        }
    }

    // --- Skirt ---
    decoded->skirtMetadata.noSkirtVerticesBegin = 0;
    decoded->skirtMetadata.noSkirtVerticesCount = vc;
    decoded->skirtMetadata.noSkirtIndicesBegin = 0;
    decoded->skirtMetadata.noSkirtIndicesCount = static_cast<uint32_t>(decoded->indices.size());
    decoded->skirtMetadata.meshCenter = decoded->localOriginEcef;

    // cesium-native skirt: partial_sort_copy each edge by UV coordinate,
    // then create bottom vertices and triangle strips.
    const double lonOff = (eastLng - westLng) * 0.0001;
    const double latOff = (northLat - southLat) * 0.0001;
    const double skirtH = calculateSkirtHeight(ellipsoid, bounds);
    decoded->skirtMetadata.skirtWestHeight = skirtH;
    decoded->skirtMetadata.skirtSouthHeight = skirtH;
    decoded->skirtMetadata.skirtEastHeight = skirtH;
    decoded->skirtMetadata.skirtNorthHeight = skirtH;

    auto addSkirtEdge = [&](const std::vector<uint32_t>& edgeIdx,
                             double lo, double la, bool reverse) {
        if (edgeIdx.size() < 2) return;
        const uint32_t firstSkirt = static_cast<uint32_t>(decoded->vertices.size());

        for (size_t i = 0; i < edgeIdx.size(); ++i) {
            size_t ei = reverse ? (edgeIdx.size() - 1 - i) : i;
            const SurfaceVertex& top = decoded->vertices[edgeIdx[ei]];
            Cartographic cart = ellipsoid.cartesianToCartographic(top.positionEcef);
            Cartographic sc = Cartographic::fromRadians(
                cart.longitude() + lo, cart.latitude() + la, cart.height() - skirtH);

            SurfaceVertex sv;
            sv.positionEcef = ellipsoid.cartographicToCartesian(sc);
            sv.normalEcef = top.normalEcef;
            sv.uv = top.uv;
            decoded->vertices.push_back(sv);
        }

        for (size_t i = 0; i + 1 < edgeIdx.size(); ++i) {
            uint32_t t0 = edgeIdx[reverse ? (edgeIdx.size() - 1 - i) : i];
            uint32_t t1 = edgeIdx[reverse ? (edgeIdx.size() - 2 - i) : (i + 1)];
            uint32_t s0 = firstSkirt + static_cast<uint32_t>(i);
            uint32_t s1 = firstSkirt + static_cast<uint32_t>(i + 1);
            decoded->indices.push_back(t0); decoded->indices.push_back(t1); decoded->indices.push_back(s0);
            decoded->indices.push_back(s0); decoded->indices.push_back(t1); decoded->indices.push_back(s1);
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
    addSkirtEdge(sortedEast,  lonOff,  0,       false);
    addSkirtEdge(sortedNorth, 0,       latOff,  false);

    decoded->waterMask = std::move(waterMask);
    return decoded;
}

} // namespace earth_engine
