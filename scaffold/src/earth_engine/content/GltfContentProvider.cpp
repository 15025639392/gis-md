#include "GltfContentProvider.h"
#include "../core/async/AsyncSystem.h"
#include "../core/cache/HttpCache.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Transforms.h"

#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#ifndef EARTH_ENGINE_HAS_LIBCURL
#if !defined(ANDROID) && __has_include(<curl/curl.h>)
#include <curl/curl.h>
#define EARTH_ENGINE_HAS_LIBCURL 1
#else
#define EARTH_ENGINE_HAS_LIBCURL 0
#endif
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace earth_engine {
namespace {

constexpr uint32_t kB3dmMagic = 0x6d643362u;
constexpr size_t kB3dmHeaderLength = 28;
constexpr size_t kB3dmLegacy1HeaderLength = 20;
constexpr size_t kB3dmLegacy2HeaderLength = 24;
constexpr uint32_t kB3dmLegacySentinel = 570425344u;
constexpr uint32_t kI3dmMagic = 0x6d643369u;
constexpr size_t kI3dmHeaderLength = 32;
constexpr uint32_t kPntsMagic = 0x73746e70u;
constexpr size_t kPntsHeaderLength = 28;
constexpr uint32_t kCmptMagic = 0x74706d63u;
constexpr size_t kCmptHeaderLength = 16;
constexpr size_t kCmptInnerHeaderLength = 12;

uint32_t readU32LE(const uint8_t* p) {
    return uint32_t(p[0]) |
           (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

uint16_t readU16LE(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

float readF32LE(const uint8_t* p) {
    float value = 0.0f;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

std::string trimRightJsonPadding(std::string value) {
    while (!value.empty() &&
           (value.back() == '\0' ||
            std::isspace(static_cast<unsigned char>(value.back())))) {
        value.pop_back();
    }
    return value;
}

std::optional<uint32_t> jsonU32(const nlohmann::json& json);

struct B3dmExtractResult {
    bool isB3dm = false;
    bool valid = false;
    const uint8_t* glbData = nullptr;
    size_t glbSize = 0;
    Mat4 rtcTransform = Mat4::identity();
};

B3dmExtractResult extractB3dmGlb(const uint8_t* data, size_t size) {
    B3dmExtractResult result;
    if (!data || size < 4 || readU32LE(data) != kB3dmMagic) {
        return result;
    }

    result.isB3dm = true;
    if (size < kB3dmHeaderLength) {
        return result;
    }

    const uint32_t version = readU32LE(data + 4);
    const uint32_t byteLength = readU32LE(data + 8);
    if (version != 1 || byteLength > size || byteLength < kB3dmHeaderLength) {
        return result;
    }

    size_t headerLength = kB3dmHeaderLength;
    uint32_t featureTableJsonByteLength = readU32LE(data + 12);
    uint32_t featureTableBinaryByteLength = readU32LE(data + 16);
    uint32_t batchTableJsonByteLength = readU32LE(data + 20);
    uint32_t batchTableBinaryByteLength = readU32LE(data + 24);

    // Match cesium-native B3dmToGltfConverter legacy header detection.
    if (batchTableJsonByteLength >= kB3dmLegacySentinel) {
        headerLength = kB3dmLegacy1HeaderLength;
        featureTableJsonByteLength = 0;
        featureTableBinaryByteLength = 0;
        batchTableJsonByteLength = readU32LE(data + 16);
        batchTableBinaryByteLength = 0;
    } else if (batchTableBinaryByteLength >= kB3dmLegacySentinel) {
        headerLength = kB3dmLegacy2HeaderLength;
        featureTableJsonByteLength = 0;
        featureTableBinaryByteLength = 0;
        batchTableJsonByteLength = readU32LE(data + 12);
        batchTableBinaryByteLength = readU32LE(data + 16);
    }

    const uint64_t glbStart64 =
        static_cast<uint64_t>(headerLength) +
        featureTableJsonByteLength +
        featureTableBinaryByteLength +
        batchTableJsonByteLength +
        batchTableBinaryByteLength;
    if (glbStart64 >= byteLength) {
        return result;
    }
    const size_t glbStart = static_cast<size_t>(glbStart64);

    if (featureTableJsonByteLength > 0) {
        if (featureTableBinaryByteLength > 0 ||
            batchTableJsonByteLength > 0 ||
            batchTableBinaryByteLength > 0) {
            return result;
        }

        const size_t featureStart = headerLength;
        const size_t featureEnd =
            featureStart + static_cast<size_t>(featureTableJsonByteLength);
        if (featureEnd > size || featureEnd > glbStart) {
            return result;
        }
        const std::string featureJson = trimRightJsonPadding(std::string(
            reinterpret_cast<const char*>(data + featureStart),
            static_cast<size_t>(featureTableJsonByteLength)));
        auto parsed = nlohmann::json::parse(featureJson, nullptr, false);
        if (parsed.is_discarded()) {
            return result;
        }
        if (parsed.contains("extensions")) {
            return result;
        }
        auto batchLengthIt = parsed.find("BATCH_LENGTH");
        if (batchLengthIt != parsed.end()) {
            std::optional<uint32_t> batchLength = jsonU32(*batchLengthIt);
            if (!batchLength || *batchLength > 0u) {
                return result;
            }
        }
        auto it = parsed.find("RTC_CENTER");
        if (it != parsed.end() && it->is_array() && it->size() == 3 &&
            (*it)[0].is_number() && (*it)[1].is_number() &&
            (*it)[2].is_number()) {
            result.rtcTransform = Mat4::translation(Vec3(
                (*it)[0].get<double>(),
                (*it)[1].get<double>(),
                (*it)[2].get<double>()));
        }
    } else if (featureTableBinaryByteLength > 0 ||
               batchTableJsonByteLength > 0 ||
               batchTableBinaryByteLength > 0) {
        return result;
    }

    result.glbData = data + glbStart;
    result.glbSize = static_cast<size_t>(byteLength) - glbStart;
    result.valid = result.glbSize > 0;
    return result;
}

#if EARTH_ENGINE_HAS_LIBCURL
std::once_flag gCurlOnce;

void ensureCurl() {
    std::call_once(gCurlOnce, [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

size_t curlWrite(void* p, size_t s, size_t n, void* u) {
    auto* body = static_cast<std::vector<uint8_t>*>(u);
    const size_t total = s * n;
    body->insert(body->end(),
                 static_cast<const uint8_t*>(p),
                 static_cast<const uint8_t*>(p) + total);
    return total;
}
#endif

std::string contentCacheKey(const TileKey& key) {
    return key.schemeId + "/" + std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" + std::to_string(key.y);
}

bool isAsciiAlpha(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z');
}

bool isAsciiAlphaNumeric(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return isAsciiAlpha(c) || (u >= '0' && u <= '9');
}

bool hasUrlScheme(const std::string& url) {
    for (size_t i = 0; i < url.size(); ++i) {
        const char c = url[i];
        if (c == ':') return i > 0;
        if (c == '/' || c == '?' || c == '#') return false;
        if ((i == 0 && !isAsciiAlpha(c)) ||
            (!isAsciiAlphaNumeric(c) && c != '+' && c != '-' && c != '.')) {
            return false;
        }
    }
    return false;
}

struct ParsedUrl {
    bool hasScheme = false;
    std::string scheme;
    bool hasAuthority = false;
    std::string authority;
    std::string path;
    bool hasQuery = false;
    std::string query;
    std::string fragment;
};

ParsedUrl parseUrl(const std::string& url) {
    ParsedUrl result;
    size_t pathStart = 0;
    if (hasUrlScheme(url)) {
        const size_t colon = url.find(':');
        result.hasScheme = true;
        result.scheme = url.substr(0, colon);
        pathStart = colon + 1;
        if (url.compare(pathStart, 2, "//") == 0) {
            result.hasAuthority = true;
            const size_t authorityStart = pathStart + 2;
            const size_t authorityEnd =
                url.find_first_of("/?#", authorityStart);
            if (authorityEnd == std::string::npos) {
                result.authority = url.substr(authorityStart);
                pathStart = url.size();
            } else {
                result.authority =
                    url.substr(authorityStart, authorityEnd - authorityStart);
                pathStart = authorityEnd;
            }
        }
    } else if (url.rfind("//", 0) == 0) {
        result.hasAuthority = true;
        const size_t authorityEnd = url.find_first_of("/?#", 2);
        if (authorityEnd == std::string::npos) {
            result.authority = url.substr(2);
            pathStart = url.size();
        } else {
            result.authority = url.substr(2, authorityEnd - 2);
            pathStart = authorityEnd;
        }
    }

    const size_t fragmentStart = url.find('#', pathStart);
    const size_t endBeforeFragment =
        fragmentStart == std::string::npos ? url.size() : fragmentStart;
    if (fragmentStart != std::string::npos) {
        result.fragment = url.substr(fragmentStart + 1);
    }

    const size_t queryStart = url.find('?', pathStart);
    if (queryStart != std::string::npos && queryStart < endBeforeFragment) {
        result.hasQuery = true;
        result.path = url.substr(pathStart, queryStart - pathStart);
        result.query = url.substr(queryStart + 1,
                                  endBeforeFragment - queryStart - 1);
    } else {
        result.path = url.substr(pathStart, endBeforeFragment - pathStart);
    }
    return result;
}

std::string composeUrl(const ParsedUrl& url) {
    std::string result;
    if (url.hasScheme) {
        result += url.scheme;
        result += ":";
    }
    if (url.hasAuthority) {
        result += "//";
        result += url.authority;
    }
    result += url.path;
    if (url.hasQuery) {
        result += "?";
        result += url.query;
    }
    if (!url.fragment.empty()) {
        result += "#";
        result += url.fragment;
    }
    return result;
}

std::string normalizePath(const std::string& path) {
    const bool absolute = !path.empty() && path.front() == '/';
    const bool trailingSlash = !path.empty() && path.back() == '/';
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const size_t end = slash == std::string::npos ? path.size() : slash;
        const std::string part = path.substr(start, end - start);
        if (part.empty() || part == ".") {
        } else if (part == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!absolute) {
                parts.push_back(part);
            }
        } else {
            parts.push_back(part);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }

    std::string result;
    if (absolute) result += "/";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += "/";
        result += parts[i];
    }
    if (trailingSlash && (result.empty() || result.back() != '/')) {
        result += "/";
    }
    if (result.empty() && absolute) result = "/";
    return result;
}

std::string baseDirectoryPath(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return "";
    return path.substr(0, slash + 1);
}

std::string queryName(const std::string& queryPart) {
    const size_t equals = queryPart.find('=');
    return equals == std::string::npos
        ? queryPart
        : queryPart.substr(0, equals);
}

std::string mergeBaseQuery(const std::string& relativeQuery,
                           const std::string& baseQuery) {
    if (baseQuery.empty()) return relativeQuery;
    if (relativeQuery.empty()) return baseQuery;

    std::unordered_set<std::string> relativeNames;
    size_t start = 0;
    while (start <= relativeQuery.size()) {
        const size_t amp = relativeQuery.find('&', start);
        const size_t end = amp == std::string::npos ? relativeQuery.size() : amp;
        relativeNames.insert(queryName(relativeQuery.substr(start, end - start)));
        if (amp == std::string::npos) break;
        start = amp + 1;
    }

    std::string result = relativeQuery;
    start = 0;
    while (start <= baseQuery.size()) {
        const size_t amp = baseQuery.find('&', start);
        const size_t end = amp == std::string::npos ? baseQuery.size() : amp;
        const std::string part = baseQuery.substr(start, end - start);
        if (!part.empty() && relativeNames.count(queryName(part)) == 0) {
            result += "&";
            result += part;
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return result;
}

std::string resolveContentUrl(const std::string& baseUrl,
                              const std::string& uri,
                              bool useBaseQuery = true) {
    ParsedUrl base = parseUrl(baseUrl);
    ParsedUrl relative = parseUrl(uri);
    ParsedUrl resolved = relative;

    if (uri.rfind("//", 0) == 0 && !relative.hasScheme) {
        resolved.hasScheme = true;
        resolved.scheme = base.hasScheme ? base.scheme : "https";
    } else if (!relative.hasScheme) {
        resolved.hasScheme = base.hasScheme;
        resolved.scheme = base.scheme;
        resolved.hasAuthority = base.hasAuthority;
        resolved.authority = base.authority;
        if (!relative.path.empty() && relative.path.front() == '/') {
            resolved.path = normalizePath(relative.path);
        } else {
            resolved.path =
                normalizePath(baseDirectoryPath(base.path) + relative.path);
        }
    }

    if (useBaseQuery) {
        const std::string mergedQuery = mergeBaseQuery(
            resolved.hasQuery ? resolved.query : std::string(),
            base.hasQuery ? base.query : std::string());
        resolved.hasQuery = !mergedQuery.empty();
        resolved.query = mergedQuery;
    }
    return composeUrl(resolved);
}

std::string toLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return value;
}

Mat4 xUpToZUpTransform() {
    return Mat4(glm::dmat4(
        glm::dvec4(0.0, 0.0, 1.0, 0.0),
        glm::dvec4(0.0, 1.0, 0.0, 0.0),
        glm::dvec4(-1.0, 0.0, 0.0, 0.0),
        glm::dvec4(0.0, 0.0, 0.0, 1.0)));
}

Mat4 yUpToZUpTransform() {
    return Mat4(glm::dmat4(
        glm::dvec4(1.0, 0.0, 0.0, 0.0),
        glm::dvec4(0.0, 0.0, 1.0, 0.0),
        glm::dvec4(0.0, -1.0, 0.0, 0.0),
        glm::dvec4(0.0, 0.0, 0.0, 1.0)));
}

Mat4 parseGltfUpAxisTransform(const nlohmann::json& tilesetJson) {
    auto assetIt = tilesetJson.find("asset");
    if (assetIt == tilesetJson.end() || !assetIt->is_object()) {
        return yUpToZUpTransform();
    }
    auto upAxisIt = assetIt->find("gltfUpAxis");
    if (upAxisIt == assetIt->end() || !upAxisIt->is_string()) {
        return yUpToZUpTransform();
    }
    const std::string upAxis = toLowerAscii(upAxisIt->get<std::string>());
    if (upAxis == "x") {
        return xUpToZUpTransform();
    }
    if (upAxis == "z") {
        return Mat4::identity();
    }
    return yUpToZUpTransform();
}

bool urlLooksLikeJson(const std::string& url) {
    ParsedUrl parsed = parseUrl(url);
    const std::string lowerPath = toLowerAscii(parsed.path);
    return lowerPath.size() >= 5 &&
           lowerPath.substr(lowerPath.size() - 5) == ".json";
}

bool urlLooksLikeGltf(const std::string& url) {
    ParsedUrl parsed = parseUrl(url);
    const std::string lowerPath = toLowerAscii(parsed.path);
    return (lowerPath.size() >= 5 &&
            lowerPath.substr(lowerPath.size() - 5) == ".gltf") ||
           (lowerPath.size() >= 4 &&
            lowerPath.substr(lowerPath.size() - 4) == ".glb") ||
           (lowerPath.size() >= 5 &&
            lowerPath.substr(lowerPath.size() - 5) == ".b3dm") ||
           (lowerPath.size() >= 5 &&
            lowerPath.substr(lowerPath.size() - 5) == ".i3dm") ||
           (lowerPath.size() >= 5 &&
            lowerPath.substr(lowerPath.size() - 5) == ".pnts") ||
           (lowerPath.size() >= 5 &&
            lowerPath.substr(lowerPath.size() - 5) == ".cmpt");
}

bool bytesLookLikeJson(const uint8_t* data, size_t size) {
    if (!data) return false;
    size_t i = 0;
    while (i < size &&
           std::isspace(static_cast<unsigned char>(data[i])) != 0) {
        ++i;
    }
    return i < size && (data[i] == '{' || data[i] == '[');
}

double maxScaleComponent(const Mat4& transform) {
    const glm::dmat4& m = transform.raw();
    return std::max(
        glm::length(glm::dvec3(m[0])),
        std::max(
            glm::length(glm::dvec3(m[1])),
            glm::length(glm::dvec3(m[2]))));
}

Vec3 transformDirection(const Mat4& transform, const Vec3& direction) {
    const glm::dvec4 v =
        transform.raw() * glm::dvec4(direction.raw(), 0.0);
    return Vec3(glm::dvec3(v));
}

TileBoundingVolume transformBoundingVolume(
    const Mat4& transform,
    const TileBoundingVolume& volume) {
    switch (volume.kind) {
        case TileBoundingVolumeKind::Region:
            return volume;
        case TileBoundingVolumeKind::Sphere: {
            const Vec3 center = transform * volume.sphere.getCenter();
            return TileBoundingVolume::fromSphere(
                center,
                volume.sphere.getRadius() * maxScaleComponent(transform));
        }
        case TileBoundingVolumeKind::Box:
            return TileBoundingVolume::fromBox(
                transform * volume.box.getCenter(),
                transformDirection(transform, volume.box.getHalfAxis(0)),
                transformDirection(transform, volume.box.getHalfAxis(1)),
                transformDirection(transform, volume.box.getHalfAxis(2)));
    }
    return volume;
}

std::optional<Mat4> parseTransform(const nlohmann::json& json) {
    if (!json.is_array() || json.size() < 16) return std::nullopt;
    double a[16] = {};
    for (int i = 0; i < 16; ++i) {
        if (!json[static_cast<size_t>(i)].is_number()) {
            return std::nullopt;
        }
        a[i] = json[static_cast<size_t>(i)].get<double>();
    }
    return Mat4(glm::dmat4(
        glm::dvec4(a[0], a[1], a[2], a[3]),
        glm::dvec4(a[4], a[5], a[6], a[7]),
        glm::dvec4(a[8], a[9], a[10], a[11]),
        glm::dvec4(a[12], a[13], a[14], a[15])));
}

std::optional<TileBoundingVolume> parseBoundingVolumeJson(
    const nlohmann::json& json) {
    if (!json.is_object()) return std::nullopt;
    auto boxIt = json.find("box");
    if (boxIt != json.end() && boxIt->is_array() && boxIt->size() >= 12) {
        double b[12] = {};
        for (int i = 0; i < 12; ++i) {
            if (!(*boxIt)[static_cast<size_t>(i)].is_number()) {
                return std::nullopt;
            }
            b[i] = (*boxIt)[static_cast<size_t>(i)].get<double>();
        }
        return TileBoundingVolume::fromBox(
            Vec3(b[0], b[1], b[2]),
            Vec3(b[3], b[4], b[5]),
            Vec3(b[6], b[7], b[8]),
            Vec3(b[9], b[10], b[11]));
    }

    auto regionIt = json.find("region");
    if (regionIt != json.end() &&
        regionIt->is_array() &&
        regionIt->size() >= 6) {
        double r[6] = {};
        for (int i = 0; i < 6; ++i) {
            if (!(*regionIt)[static_cast<size_t>(i)].is_number()) {
                return std::nullopt;
            }
            r[i] = (*regionIt)[static_cast<size_t>(i)].get<double>();
        }
        return TileBoundingVolume::fromRegion(
            Rectangle(r[0], r[1], r[2], r[3]),
            r[4],
            r[5]);
    }

    auto sphereIt = json.find("sphere");
    if (sphereIt != json.end() &&
        sphereIt->is_array() &&
        sphereIt->size() >= 4) {
        double s[4] = {};
        for (int i = 0; i < 4; ++i) {
            if (!(*sphereIt)[static_cast<size_t>(i)].is_number()) {
                return std::nullopt;
            }
            s[i] = (*sphereIt)[static_cast<size_t>(i)].get<double>();
        }
        return TileBoundingVolume::fromSphere(
            Vec3(s[0], s[1], s[2]),
            s[3]);
    }

    return std::nullopt;
}

Rectangle boundsFromVolumeOrWorld(
    const std::optional<TileBoundingVolume>& volume) {
    if (volume && volume->kind == TileBoundingVolumeKind::Region) {
        return volume->region;
    }
    return Rectangle(
        -3.14159265358979323846264338327950288,
        -1.57079632679489661923132169163975144,
        3.14159265358979323846264338327950288,
        1.57079632679489661923132169163975144);
}

std::optional<std::string> parseContentUri(const nlohmann::json& tileJson) {
    auto contentIt = tileJson.find("content");
    if (contentIt == tileJson.end() || !contentIt->is_object()) {
        return std::nullopt;
    }
    auto uriIt = contentIt->find("uri");
    if (uriIt != contentIt->end() && uriIt->is_string()) {
        return uriIt->get<std::string>();
    }
    auto urlIt = contentIt->find("url");
    if (urlIt != contentIt->end() && urlIt->is_string()) {
        return urlIt->get<std::string>();
    }
    return std::string{};
}

std::optional<TileBoundingVolume> parseContentBoundingVolume(
    const nlohmann::json& tileJson,
    const Mat4& transform) {
    auto contentIt = tileJson.find("content");
    if (contentIt == tileJson.end() || !contentIt->is_object()) {
        return std::nullopt;
    }
    auto bvIt = contentIt->find("boundingVolume");
    if (bvIt == contentIt->end()) {
        return std::nullopt;
    }
    std::optional<TileBoundingVolume> volume =
        parseBoundingVolumeJson(*bvIt);
    if (!volume) return std::nullopt;
    return transformBoundingVolume(transform, *volume);
}

std::optional<TileBoundingVolume> parseViewerRequestVolume(
    const nlohmann::json& tileJson,
    const Mat4& transform) {
    auto bvIt = tileJson.find("viewerRequestVolume");
    if (bvIt == tileJson.end()) {
        return std::nullopt;
    }
    std::optional<TileBoundingVolume> volume =
        parseBoundingVolumeJson(*bvIt);
    if (!volume) return std::nullopt;
    return transformBoundingVolume(transform, *volume);
}

bool isSupportedTilesetJsonExtension(const std::string& extensionName) {
    return extensionName == "3DTILES_content_gltf";
}

bool canFailTilesetJsonExtensionPerTile(const std::string& extensionName) {
    return extensionName == "3DTILES_implicit_tiling" ||
           extensionName == "3DTILES_multiple_contents";
}

bool tilesetJsonExtensionAllowed(const std::string& extensionName,
                                 bool allowPerTileFailure) {
    return isSupportedTilesetJsonExtension(extensionName) ||
           (allowPerTileFailure &&
            canFailTilesetJsonExtensionPerTile(extensionName));
}

bool hasUnsupportedDeclaredExtensionsOnObject(
    const nlohmann::json& json,
    bool allowPerTileFailure = false) {
    auto checkArray = [&](const char* name) {
        auto extensionsIt = json.find(name);
        if (extensionsIt == json.end()) {
            return false;
        }
        if (!extensionsIt->is_array()) {
            return true;
        }
        for (const auto& extension : *extensionsIt) {
            if (!extension.is_string()) {
                return true;
            }
            if (!tilesetJsonExtensionAllowed(
                    extension.get<std::string>(),
                    allowPerTileFailure)) {
                return true;
            }
        }
        return false;
    };

    return checkArray("extensionsRequired") ||
           checkArray("extensionsUsed");
}

bool hasUnsupportedDeclaredExtensionsInTileObject(
    const nlohmann::json& value) {
    if (value.is_object()) {
        if (hasUnsupportedDeclaredExtensionsOnObject(value)) {
            return true;
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.key() == "children" || it.key() == "extensions") {
                continue;
            }
            if (hasUnsupportedDeclaredExtensionsInTileObject(*it)) {
                return true;
            }
        }
    } else if (value.is_array()) {
        for (const auto& element : value) {
            if (hasUnsupportedDeclaredExtensionsInTileObject(element)) {
                return true;
            }
        }
    }
    return false;
}

bool extensionsObjectHasUnsupportedTilesetExtension(
    const nlohmann::json& extensions,
    bool allowPerTileFailure) {
    if (!extensions.is_object()) {
        return true;
    }
    for (auto it = extensions.begin(); it != extensions.end(); ++it) {
        if (!tilesetJsonExtensionAllowed(it.key(), allowPerTileFailure)) {
            return true;
        }
    }
    return false;
}

bool hasUnsupportedDirectExtensionsObject(
    const nlohmann::json& value,
    bool allowPerTileFailure = false) {
    auto extensionsIt = value.find("extensions");
    return extensionsIt != value.end() &&
           extensionsObjectHasUnsupportedTilesetExtension(
               *extensionsIt,
               allowPerTileFailure);
}

bool hasUnsupportedExtensionsObjectInTileObject(
    const nlohmann::json& value) {
    if (value.is_object()) {
        if (hasUnsupportedDirectExtensionsObject(value)) {
            return true;
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.key() == "children" || it.key() == "extensions") {
                continue;
            }
            if (hasUnsupportedExtensionsObjectInTileObject(*it)) {
                return true;
            }
        }
    } else if (value.is_array()) {
        for (const auto& element : value) {
            if (hasUnsupportedExtensionsObjectInTileObject(element)) {
                return true;
            }
        }
    }
    return false;
}

bool hasUnsupportedMultipleContents(const nlohmann::json& tileJson) {
    auto contentsIt = tileJson.find("contents");
    if (contentsIt != tileJson.end() && contentsIt->is_array() &&
        !contentsIt->empty()) {
        return true;
    }

    auto extensionsIt = tileJson.find("extensions");
    if (extensionsIt == tileJson.end() || !extensionsIt->is_object()) {
        return false;
    }
    return extensionsIt->find("3DTILES_multiple_contents") !=
           extensionsIt->end();
}

bool hasUnsupportedImplicitTiling(const nlohmann::json& tileJson) {
    auto implicitTilingIt = tileJson.find("implicitTiling");
    if (implicitTilingIt != tileJson.end()) {
        return true;
    }

    auto extensionsIt = tileJson.find("extensions");
    if (extensionsIt == tileJson.end() || !extensionsIt->is_object()) {
        return false;
    }
    return extensionsIt->find("3DTILES_implicit_tiling") !=
           extensionsIt->end();
}

bool hasUnsupportedTileRequiredExtensions(const nlohmann::json& tileJson) {
    if (hasUnsupportedDeclaredExtensionsInTileObject(tileJson) ||
        hasUnsupportedExtensionsObjectInTileObject(tileJson)) {
        return true;
    }

    auto contentIt = tileJson.find("content");
    return contentIt != tileJson.end() && contentIt->is_object() &&
           (hasUnsupportedDeclaredExtensionsInTileObject(*contentIt) ||
            hasUnsupportedExtensionsObjectInTileObject(*contentIt));
}

struct I3dmHeader {
    uint32_t version = 0;
    uint32_t byteLength = 0;
    uint32_t featureTableJsonByteLength = 0;
    uint32_t featureTableBinaryByteLength = 0;
    uint32_t batchTableJsonByteLength = 0;
    uint32_t batchTableBinaryByteLength = 0;
    uint32_t gltfFormat = 0;
};

struct I3dmFeatureTable {
    uint32_t instancesLength = 0;
    std::optional<glm::dvec3> rtcCenter;
    std::optional<glm::dvec3> quantizedVolumeOffset;
    std::optional<glm::dvec3> quantizedVolumeScale;
    bool eastNorthUp = false;
    std::optional<uint32_t> position;
    std::optional<uint32_t> positionQuantized;
    std::optional<uint32_t> normalUp;
    std::optional<uint32_t> normalRight;
    std::optional<uint32_t> normalUpOct32p;
    std::optional<uint32_t> normalRightOct32p;
    std::optional<uint32_t> scale;
    std::optional<uint32_t> scaleNonUniform;
};

struct DecodedI3dmInstances {
    std::vector<glm::dvec3> positions;
    std::vector<glm::dquat> rotations;
    std::vector<glm::dvec3> scales;
    std::optional<glm::dvec3> rtcCenter;
};

struct PntsHeader {
    uint32_t version = 0;
    uint32_t byteLength = 0;
    uint32_t featureTableJsonByteLength = 0;
    uint32_t featureTableBinaryByteLength = 0;
    uint32_t batchTableJsonByteLength = 0;
    uint32_t batchTableBinaryByteLength = 0;
};

struct CmptHeader {
    uint32_t version = 0;
    uint32_t byteLength = 0;
    uint32_t tilesLength = 0;
};

bool checkedRange(size_t bufferSize,
                  uint32_t offset,
                  uint32_t count,
                  size_t stride) {
    const uint64_t begin = offset;
    const uint64_t byteLength =
        static_cast<uint64_t>(count) * static_cast<uint64_t>(stride);
    return begin <= bufferSize && byteLength <= bufferSize - begin;
}

std::optional<uint32_t> jsonU32(const nlohmann::json& json) {
    if (json.is_number_unsigned()) {
        const uint64_t value = json.get<uint64_t>();
        if (value <= std::numeric_limits<uint32_t>::max()) {
            return static_cast<uint32_t>(value);
        }
    }
    if (json.is_number_integer()) {
        const int64_t value = json.get<int64_t>();
        if (value >= 0 &&
            static_cast<uint64_t>(value) <=
                std::numeric_limits<uint32_t>::max()) {
            return static_cast<uint32_t>(value);
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> semanticOffset(const nlohmann::json& featureJson,
                                       const char* semantic,
                                       bool& valid) {
    auto it = featureJson.find(semantic);
    if (it == featureJson.end()) return std::nullopt;
    if (!it->is_object()) return std::nullopt;

    auto offsetIt = it->find("byteOffset");
    if (offsetIt == it->end()) {
        valid = false;
        return std::nullopt;
    }
    std::optional<uint32_t> offset = jsonU32(*offsetIt);
    if (!offset) {
        valid = false;
        return std::nullopt;
    }
    return offset;
}

bool finiteVec3(const glm::dvec3& value) {
    return std::isfinite(value.x) &&
           std::isfinite(value.y) &&
           std::isfinite(value.z);
}

std::optional<glm::dvec3> jsonVec3(const nlohmann::json& json,
                                   const char* name,
                                   bool& valid) {
    auto it = json.find(name);
    if (it == json.end()) {
        return std::nullopt;
    }
    if (!it->is_array() || it->size() != 3) {
        valid = false;
        return std::nullopt;
    }
    if (!(*it)[0].is_number() ||
        !(*it)[1].is_number() ||
        !(*it)[2].is_number()) {
        valid = false;
        return std::nullopt;
    }
    glm::dvec3 value(
        (*it)[0].get<double>(),
        (*it)[1].get<double>(),
        (*it)[2].get<double>());
    if (!finiteVec3(value)) {
        valid = false;
        return std::nullopt;
    }
    return value;
}

std::optional<I3dmHeader> parseI3dmHeader(const uint8_t* data, size_t size) {
    if (!data || size < 4 || readU32LE(data) != kI3dmMagic) {
        return std::nullopt;
    }
    if (size < kI3dmHeaderLength) {
        return std::nullopt;
    }

    I3dmHeader header;
    header.version = readU32LE(data + 4);
    header.byteLength = readU32LE(data + 8);
    header.featureTableJsonByteLength = readU32LE(data + 12);
    header.featureTableBinaryByteLength = readU32LE(data + 16);
    header.batchTableJsonByteLength = readU32LE(data + 20);
    header.batchTableBinaryByteLength = readU32LE(data + 24);
    header.gltfFormat = readU32LE(data + 28);

    if (header.version != 1 ||
        header.byteLength > size ||
        header.byteLength < kI3dmHeaderLength) {
        return std::nullopt;
    }

    uint64_t offset = kI3dmHeaderLength;
    const uint32_t sectionLengths[] = {
        header.featureTableJsonByteLength,
        header.featureTableBinaryByteLength,
        header.batchTableJsonByteLength,
        header.batchTableBinaryByteLength};
    for (uint32_t sectionLength : sectionLengths) {
        offset += sectionLength;
        if (offset > header.byteLength) {
            return std::nullopt;
        }
    }
    return header;
}

std::optional<I3dmFeatureTable> parseI3dmFeatureTable(
    const uint8_t* data,
    const I3dmHeader& header) {
    if (header.featureTableJsonByteLength == 0 ||
        header.featureTableBinaryByteLength == 0) {
        return std::nullopt;
    }

    const std::string featureJsonText = trimRightJsonPadding(std::string(
        reinterpret_cast<const char*>(data + kI3dmHeaderLength),
        header.featureTableJsonByteLength));
    auto featureJson =
        nlohmann::json::parse(featureJsonText, nullptr, false);
    if (featureJson.is_discarded() || !featureJson.is_object()) {
        return std::nullopt;
    }
    if (featureJson.contains("extensions") ||
        featureJson.contains("BATCH_ID")) {
        return std::nullopt;
    }

    auto instancesIt = featureJson.find("INSTANCES_LENGTH");
    if (instancesIt == featureJson.end()) return std::nullopt;
    std::optional<uint32_t> instancesLength = jsonU32(*instancesIt);
    if (!instancesLength) return std::nullopt;

    I3dmFeatureTable table;
    table.instancesLength = *instancesLength;
    bool valid = true;
    table.rtcCenter = jsonVec3(featureJson, "RTC_CENTER", valid);
    table.quantizedVolumeOffset =
        jsonVec3(featureJson, "QUANTIZED_VOLUME_OFFSET", valid);
    table.quantizedVolumeScale =
        jsonVec3(featureJson, "QUANTIZED_VOLUME_SCALE", valid);

    auto enuIt = featureJson.find("EAST_NORTH_UP");
    if (enuIt != featureJson.end()) {
        if (!enuIt->is_boolean()) {
            valid = false;
        } else {
            table.eastNorthUp = enuIt->get<bool>();
        }
    }

    table.position = semanticOffset(featureJson, "POSITION", valid);
    table.positionQuantized =
        semanticOffset(featureJson, "POSITION_QUANTIZED", valid);
    table.normalUp = semanticOffset(featureJson, "NORMAL_UP", valid);
    table.normalRight = semanticOffset(featureJson, "NORMAL_RIGHT", valid);
    table.normalUpOct32p =
        semanticOffset(featureJson, "NORMAL_UP_OCT32P", valid);
    table.normalRightOct32p =
        semanticOffset(featureJson, "NORMAL_RIGHT_OCT32P", valid);
    table.scale = semanticOffset(featureJson, "SCALE", valid);
    table.scaleNonUniform =
        semanticOffset(featureJson, "SCALE_NON_UNIFORM", valid);
    if (!valid) return std::nullopt;

    if (!table.position && !table.positionQuantized) return std::nullopt;
    if (table.positionQuantized &&
        (!table.quantizedVolumeOffset || !table.quantizedVolumeScale)) {
        return std::nullopt;
    }
    if (table.normalUp.has_value() != table.normalRight.has_value()) {
        return std::nullopt;
    }
    if (table.normalUpOct32p.has_value() !=
        table.normalRightOct32p.has_value()) {
        return std::nullopt;
    }
    return table;
}

glm::dvec3 readFeatureVec3F32(const uint8_t* binary,
                              uint32_t offset,
                              uint32_t index) {
    const uint8_t* p = binary + offset + static_cast<size_t>(index) * 12u;
    return glm::dvec3(readF32LE(p), readF32LE(p + 4), readF32LE(p + 8));
}

glm::dvec3 readFeatureVec3U16(const uint8_t* binary,
                              uint32_t offset,
                              uint32_t index) {
    const uint8_t* p = binary + offset + static_cast<size_t>(index) * 6u;
    return glm::dvec3(
        readU16LE(p),
        readU16LE(p + 2),
        readU16LE(p + 4));
}

glm::dvec3 octDecodeInRange(uint16_t x, uint16_t y) {
    constexpr double range = 65535.0;
    glm::dvec3 result(
        double(x) / range * 2.0 - 1.0,
        double(y) / range * 2.0 - 1.0,
        0.0);
    result.z = 1.0 - std::abs(result.x) - std::abs(result.y);
    if (result.z < 0.0) {
        const double oldX = result.x;
        const auto signNotZero = [](double value) {
            return value < 0.0 ? -1.0 : 1.0;
        };
        result.x = (1.0 - std::abs(result.y)) * signNotZero(oldX);
        result.y = (1.0 - std::abs(oldX)) * signNotZero(result.y);
    }
    const double length = glm::length(result);
    return length > 0.0 ? result / length : glm::dvec3(0.0, 0.0, 1.0);
}

glm::dquat rotationFromUpRight(glm::dvec3 up, glm::dvec3 right) {
    if (glm::length(up) <= 0.0 || glm::length(right) <= 0.0) {
        return glm::dquat(1.0, 0.0, 0.0, 0.0);
    }
    up = glm::normalize(up);
    right = glm::normalize(right);
    glm::dvec3 forward = glm::cross(right, up);
    if (glm::length(forward) <= 0.0) {
        return glm::dquat(1.0, 0.0, 0.0, 0.0);
    }
    forward = glm::normalize(forward);
    return glm::normalize(glm::quat_cast(glm::dmat3(right, up, forward)));
}

void recenterI3dmInstances(DecodedI3dmInstances& instances) {
    if (instances.positions.empty()) return;

    glm::dvec3 center(0.0);
    for (const glm::dvec3& position : instances.positions) {
        center += position;
    }
    center /= static_cast<double>(instances.positions.size());

    for (glm::dvec3& position : instances.positions) {
        position -= center;
    }
    if (instances.rtcCenter) {
        *instances.rtcCenter += center;
    } else {
        instances.rtcCenter = center;
    }
}

std::optional<DecodedI3dmInstances> decodeI3dmInstances(
    const uint8_t* data,
    const I3dmHeader& header,
    const Mat4& tileTransform) {
    std::optional<I3dmFeatureTable> table =
        parseI3dmFeatureTable(data, header);
    if (!table) return std::nullopt;

    const uint8_t* binary =
        data + kI3dmHeaderLength + header.featureTableJsonByteLength;
    const size_t binarySize = header.featureTableBinaryByteLength;
    const uint32_t count = table->instancesLength;

    if (table->position &&
        !checkedRange(binarySize, *table->position, count, 12)) {
        return std::nullopt;
    }
    if (table->positionQuantized &&
        !checkedRange(binarySize, *table->positionQuantized, count, 6)) {
        return std::nullopt;
    }
    if (table->normalUp &&
        !checkedRange(binarySize, *table->normalUp, count, 12)) {
        return std::nullopt;
    }
    if (table->normalRight &&
        !checkedRange(binarySize, *table->normalRight, count, 12)) {
        return std::nullopt;
    }
    if (table->normalUpOct32p &&
        !checkedRange(binarySize, *table->normalUpOct32p, count, 4)) {
        return std::nullopt;
    }
    if (table->normalRightOct32p &&
        !checkedRange(binarySize, *table->normalRightOct32p, count, 4)) {
        return std::nullopt;
    }
    if (table->scale &&
        !checkedRange(binarySize, *table->scale, count, 4)) {
        return std::nullopt;
    }
    if (table->scaleNonUniform &&
        !checkedRange(binarySize, *table->scaleNonUniform, count, 12)) {
        return std::nullopt;
    }

    DecodedI3dmInstances instances;
    instances.rtcCenter = table->rtcCenter;
    instances.positions.resize(count, glm::dvec3(0.0));
    instances.rotations.resize(count, glm::dquat(1.0, 0.0, 0.0, 0.0));
    instances.scales.resize(count, glm::dvec3(1.0));

    if (table->position) {
        for (uint32_t i = 0; i < count; ++i) {
            const glm::dvec3 position =
                readFeatureVec3F32(binary, *table->position, i);
            if (!finiteVec3(position)) {
                return std::nullopt;
            }
            instances.positions[i] = position;
        }
    } else if (table->positionQuantized) {
        for (uint32_t i = 0; i < count; ++i) {
            const glm::dvec3 q =
                readFeatureVec3U16(binary, *table->positionQuantized, i);
            instances.positions[i] =
                q / 65535.0 * *table->quantizedVolumeScale +
                *table->quantizedVolumeOffset;
        }
    }

    if (table->normalUp && table->normalRight) {
        for (uint32_t i = 0; i < count; ++i) {
            const glm::dvec3 up =
                readFeatureVec3F32(binary, *table->normalUp, i);
            const glm::dvec3 right =
                readFeatureVec3F32(binary, *table->normalRight, i);
            if (!finiteVec3(up) || !finiteVec3(right)) {
                return std::nullopt;
            }
            instances.rotations[i] = rotationFromUpRight(
                up,
                right);
        }
    } else if (table->normalUpOct32p && table->normalRightOct32p) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* up =
                binary + *table->normalUpOct32p + static_cast<size_t>(i) * 4u;
            const uint8_t* right =
                binary + *table->normalRightOct32p +
                static_cast<size_t>(i) * 4u;
            instances.rotations[i] = rotationFromUpRight(
                octDecodeInRange(readU16LE(up), readU16LE(up + 2)),
                octDecodeInRange(readU16LE(right), readU16LE(right + 2)));
        }
    } else if (table->eastNorthUp) {
        glm::dmat4 worldTransform = tileTransform.raw();
        if (table->rtcCenter) {
            worldTransform =
                glm::translate(worldTransform, *table->rtcCenter);
        }
        const glm::dmat4 worldTransformInv = glm::inverse(worldTransform);
        for (uint32_t i = 0; i < count; ++i) {
            const glm::dvec4 worldPos4 =
                worldTransform * glm::dvec4(instances.positions[i], 1.0);
            const glm::dvec3 worldPos =
                glm::dvec3(worldPos4) / worldPos4.w;
            const Mat4 enu =
                Transforms::eastNorthUpToFixedFrame(Vec3(worldPos));
            const glm::dmat4 tileFrame =
                worldTransformInv * enu.raw();
            instances.rotations[i] = rotationFromUpRight(
                glm::dvec3(tileFrame[1]),
                glm::dvec3(tileFrame[0]));
        }
    }

    if (table->scale) {
        for (uint32_t i = 0; i < count; ++i) {
            const float s =
                readF32LE(binary + *table->scale + static_cast<size_t>(i) * 4u);
            if (!std::isfinite(s)) {
                return std::nullopt;
            }
            instances.scales[i] = glm::dvec3(s);
        }
    }
    if (table->scaleNonUniform) {
        for (uint32_t i = 0; i < count; ++i) {
            const glm::dvec3 scale =
                readFeatureVec3F32(binary, *table->scaleNonUniform, i);
            if (!finiteVec3(scale)) {
                return std::nullopt;
            }
            instances.scales[i] *= scale;
        }
    }

    recenterI3dmInstances(instances);
    return instances;
}

std::vector<GltfInstance> makeGltfInstances(
    const DecodedI3dmInstances& decoded,
    const Mat4& gltfUpAxisTransform) {
    std::vector<GltfInstance> instances;
    instances.reserve(decoded.positions.size());
    for (size_t i = 0; i < decoded.positions.size(); ++i) {
        glm::dmat4 transform(1.0);
        transform = glm::translate(transform, decoded.positions[i]);
        transform = transform * glm::mat4_cast(decoded.rotations[i]);
        transform = glm::scale(transform, decoded.scales[i]);
        GltfInstance instance;
        instance.transform = Mat4(transform) * gltfUpAxisTransform;
        instances.push_back(instance);
    }
    return instances;
}

std::optional<std::vector<GltfInstance>> combineI3dmAndNativeGltfInstances(
    const std::vector<GltfInstance>& i3dmInstances,
    const std::vector<GltfInstance>& nativeInstances) {
    if (nativeInstances.empty()) {
        return i3dmInstances;
    }
    if (i3dmInstances.size() >
            std::numeric_limits<size_t>::max() / nativeInstances.size()) {
        return std::nullopt;
    }
    std::vector<GltfInstance> combined;
    combined.reserve(i3dmInstances.size() * nativeInstances.size());
    for (const GltfInstance& i3dmInstance : i3dmInstances) {
        for (const GltfInstance& nativeInstance : nativeInstances) {
            GltfInstance instance;
            instance.transform =
                i3dmInstance.transform * nativeInstance.transform;
            combined.push_back(instance);
        }
    }
    return combined;
}

GltfParser::ImageDecoder makeImageDecoder(PlatformBridge* platformBridge) {
    if (!platformBridge) {
        return GltfParser::ImageDecoder{};
    }

    return [platformBridge](const uint8_t* data,
                            size_t size) -> std::optional<GltfImage> {
        std::unique_ptr<DecodedImage> decoded =
            platformBridge->decodeImage(data, size);
        if (!decoded || decoded->width <= 0 || decoded->height <= 0 ||
            (decoded->channels != 3 && decoded->channels != 4) ||
            decoded->pixels.empty()) {
            return std::nullopt;
        }

        const size_t expectedSize =
            static_cast<size_t>(decoded->width) *
            static_cast<size_t>(decoded->height) *
            static_cast<size_t>(decoded->channels);
        if (decoded->pixels.size() < expectedSize) {
            return std::nullopt;
        }
        if (decoded->pixels.size() > expectedSize) {
            decoded->pixels.resize(expectedSize);
        }

        GltfImage image;
        image.width = decoded->width;
        image.height = decoded->height;
        image.channels = decoded->channels;
        image.pixels = std::move(decoded->pixels);
        return image;
    };
}

std::string trimRightSpaces(std::string value) {
    while (!value.empty() && value.back() == ' ') {
        value.pop_back();
    }
    return value;
}

TileContentLoadResult decodeI3dmContent(
    const uint8_t* data,
    size_t size,
    const Mat4& baseTransform,
    const Mat4& gltfUpAxisTransform,
    const std::string& contentUrl,
    const GltfParser::ExternalResourceResolver& resolver,
    const GltfParser::ImageDecoder& imageDecoder) {
    std::optional<I3dmHeader> header = parseI3dmHeader(data, size);
    if (!header) return TileContentLoadResult::failed();
    if (header->batchTableJsonByteLength > 0 ||
        header->batchTableBinaryByteLength > 0) {
        return TileContentLoadResult::failed();
    }

    std::optional<DecodedI3dmInstances> decoded =
        decodeI3dmInstances(data, *header, baseTransform);
    if (!decoded) return TileContentLoadResult::failed();

    const size_t gltfStart =
        kI3dmHeaderLength +
        header->featureTableJsonByteLength +
        header->featureTableBinaryByteLength +
        header->batchTableJsonByteLength +
        header->batchTableBinaryByteLength;
    if (gltfStart >= header->byteLength) {
        return TileContentLoadResult::failed();
    }

    std::unique_ptr<GltfModel> model;
    if (header->gltfFormat == 1) {
        GltfParser::ExternalResourceResolver embeddedResolver;
        if (resolver && !contentUrl.empty()) {
            embeddedResolver = [resolver, contentUrl](const std::string& uri) {
                return resolver(resolveContentUrl(contentUrl, uri, false));
            };
        } else {
            embeddedResolver = resolver;
        }
        model = GltfParser::parse(
            data + gltfStart,
            static_cast<size_t>(header->byteLength) - gltfStart,
            embeddedResolver,
            imageDecoder);
    } else if (header->gltfFormat == 0) {
        if (!resolver) return TileContentLoadResult::failed();
        const std::string gltfUri = trimRightSpaces(std::string(
            reinterpret_cast<const char*>(data + gltfStart),
            static_cast<size_t>(header->byteLength) - gltfStart));
        if (gltfUri.empty()) return TileContentLoadResult::failed();
        const std::string gltfUrl = contentUrl.empty()
            ? gltfUri
            : resolveContentUrl(contentUrl, gltfUri, false);
        std::vector<uint8_t> gltfBytes = resolver(gltfUrl);
        if (gltfBytes.empty()) return TileContentLoadResult::failed();
        GltfParser::ExternalResourceResolver nestedResolver =
            [resolver, gltfUrl](const std::string& uri) {
                return resolver(resolveContentUrl(gltfUrl, uri, false));
            };
        model = GltfParser::parse(
            gltfBytes.data(),
            gltfBytes.size(),
            nestedResolver,
            imageDecoder);
    } else {
        return TileContentLoadResult::failed();
    }

    if (!model || model->primitives.empty()) {
        return TileContentLoadResult::failed();
    }

    std::vector<GltfInstance> instances =
        makeGltfInstances(*decoded, gltfUpAxisTransform);
    for (GltfPrimitive& primitive : model->primitives) {
        std::optional<std::vector<GltfInstance>> combined =
            combineI3dmAndNativeGltfInstances(
                instances,
                primitive.instances);
        if (!combined) {
            return TileContentLoadResult::failed();
        }
        primitive.instances = std::move(*combined);
    }

    TileContentLoadResult result =
        TileContentLoadResult::render(std::move(model));
    result.contentTransform = baseTransform;
    if (decoded->rtcCenter) {
        result.contentTransform =
            result.contentTransform *
            Mat4::translation(Vec3(*decoded->rtcCenter));
    }
    return result;
}

TileContentLoadResult decodeGltfLikeContent(
    const uint8_t* data,
    size_t size,
    const Mat4& baseTransform,
    const Mat4& gltfUpAxisTransform,
    const std::string& contentUrl,
    const GltfParser::ExternalResourceResolver& resolver,
    const GltfParser::ImageDecoder& imageDecoder);

std::optional<PntsHeader> parsePntsHeader(const uint8_t* data, size_t size) {
    if (!data || size < 4 || readU32LE(data) != kPntsMagic) {
        return std::nullopt;
    }
    if (size < kPntsHeaderLength) {
        return std::nullopt;
    }

    PntsHeader header;
    header.version = readU32LE(data + 4);
    header.byteLength = readU32LE(data + 8);
    header.featureTableJsonByteLength = readU32LE(data + 12);
    header.featureTableBinaryByteLength = readU32LE(data + 16);
    header.batchTableJsonByteLength = readU32LE(data + 20);
    header.batchTableBinaryByteLength = readU32LE(data + 24);

    if (header.version != 1 ||
        header.byteLength > size ||
        header.byteLength < kPntsHeaderLength) {
        return std::nullopt;
    }

    uint64_t offset = kPntsHeaderLength;
    const uint32_t sectionLengths[] = {
        header.featureTableJsonByteLength,
        header.featureTableBinaryByteLength,
        header.batchTableJsonByteLength,
        header.batchTableBinaryByteLength};
    for (uint32_t sectionLength : sectionLengths) {
        offset += sectionLength;
        if (offset > header.byteLength) {
            return std::nullopt;
        }
    }
    return header;
}

std::optional<std::array<float, 4>> pntsConstantRgba(
    const nlohmann::json& featureJson) {
    auto it = featureJson.find("CONSTANT_RGBA");
    if (it == featureJson.end()) {
        return std::nullopt;
    }
    if (!it->is_array() || it->size() != 4) {
        return std::nullopt;
    }
    std::array<float, 4> color{};
    for (size_t i = 0; i < 4; ++i) {
        std::optional<uint32_t> component = jsonU32((*it)[i]);
        if (!component || *component > 255u) {
            return std::nullopt;
        }
        const float normalized =
            static_cast<float>(*component) / 255.0f;
        color[i] = i < 3 ? std::pow(normalized, 2.2f) : normalized;
    }
    return color;
}

float pntsSrgbByteToLinear(uint8_t value) {
    return std::pow(static_cast<float>(value) / 255.0f, 2.2f);
}

std::unique_ptr<GltfModel> parsePntsModel(const uint8_t* data,
                                          const PntsHeader& header) {
    if (header.featureTableJsonByteLength == 0 ||
        header.batchTableJsonByteLength != 0 ||
        header.batchTableBinaryByteLength != 0) {
        return nullptr;
    }

    const uint8_t* featureJsonBytes = data + kPntsHeaderLength;
    const uint8_t* featureBinary =
        featureJsonBytes + header.featureTableJsonByteLength;
    const size_t featureBinarySize = header.featureTableBinaryByteLength;
    const std::string featureJsonText = trimRightJsonPadding(std::string(
        reinterpret_cast<const char*>(featureJsonBytes),
        header.featureTableJsonByteLength));
    auto featureJson =
        nlohmann::json::parse(featureJsonText, nullptr, false);
    if (featureJson.is_discarded() || !featureJson.is_object()) {
        return nullptr;
    }

    if (featureJson.contains("extensions") ||
        featureJson.contains("POSITION_QUANTIZED") ||
        featureJson.contains("RGB565") ||
        featureJson.contains("NORMAL") ||
        featureJson.contains("NORMAL_OCT16P") ||
        featureJson.contains("BATCH_ID") ||
        featureJson.contains("BATCH_LENGTH")) {
        return nullptr;
    }

    auto pointsLengthIt = featureJson.find("POINTS_LENGTH");
    if (pointsLengthIt == featureJson.end()) {
        return nullptr;
    }
    std::optional<uint32_t> pointsLength = jsonU32(*pointsLengthIt);
    if (!pointsLength) {
        return nullptr;
    }
    if (*pointsLength == 0) {
        return std::make_unique<GltfModel>();
    }

    bool valid = true;
    const std::optional<uint32_t> positionOffset =
        semanticOffset(featureJson, "POSITION", valid);
    if (!valid || !positionOffset ||
        !checkedRange(
            featureBinarySize,
            *positionOffset,
            *pointsLength,
            12u)) {
        return nullptr;
    }

    const std::optional<glm::dvec3> rtcCenter =
        jsonVec3(featureJson, "RTC_CENTER", valid);
    if (!valid) {
        return nullptr;
    }

    std::optional<uint32_t> rgbOffset =
        semanticOffset(featureJson, "RGB", valid);
    if (!valid) {
        return nullptr;
    }
    std::optional<uint32_t> rgbaOffset =
        semanticOffset(featureJson, "RGBA", valid);
    if (!valid) {
        return nullptr;
    }
    if (rgbOffset && rgbaOffset) {
        return nullptr;
    }
    if (rgbOffset &&
        !checkedRange(featureBinarySize, *rgbOffset, *pointsLength, 3u)) {
        return nullptr;
    }
    if (rgbaOffset &&
        !checkedRange(featureBinarySize, *rgbaOffset, *pointsLength, 4u)) {
        return nullptr;
    }

    std::optional<std::array<float, 4>> constantColor =
        pntsConstantRgba(featureJson);
    if (featureJson.contains("CONSTANT_RGBA") && !constantColor) {
        return nullptr;
    }

    auto model = std::make_unique<GltfModel>();
    GltfPrimitive primitive;
    primitive.primitiveMode = GltfPrimitiveMode::Points;
    primitive.metallicFactor = 0.0f;
    primitive.roughnessFactor = 0.9f;
    primitive.unlit = true;
    if (constantColor) {
        primitive.baseColorFactor = *constantColor;
        primitive.alphaMode = GltfAlphaMode::Blend;
    }

    primitive.vertices.reserve(*pointsLength);
    primitive.indices.reserve(*pointsLength);
    if (rgbOffset || rgbaOffset) {
        primitive.vertexColors.reserve(*pointsLength);
        if (rgbaOffset) {
            primitive.alphaMode = GltfAlphaMode::Blend;
        }
    }

    for (uint32_t i = 0; i < *pointsLength; ++i) {
        const uint8_t* p =
            featureBinary + *positionOffset + static_cast<size_t>(i) * 12u;
        glm::dvec3 position(
            readF32LE(p),
            readF32LE(p + 4),
            readF32LE(p + 8));
        if (!finiteVec3(position)) {
            return nullptr;
        }
        if (rtcCenter) {
            position += *rtcCenter;
        }

        SurfaceVertex vertex;
        vertex.positionEcef = Vec3(position);
        vertex.normalEcef = Vec3::unitZ();
        primitive.vertices.push_back(vertex);
        primitive.indices.push_back(i);

        if (rgbOffset) {
            const uint8_t* c =
                featureBinary + *rgbOffset + static_cast<size_t>(i) * 3u;
            primitive.vertexColors.push_back({
                pntsSrgbByteToLinear(c[0]),
                pntsSrgbByteToLinear(c[1]),
                pntsSrgbByteToLinear(c[2]),
                1.0f});
        } else if (rgbaOffset) {
            const uint8_t* c =
                featureBinary + *rgbaOffset + static_cast<size_t>(i) * 4u;
            primitive.vertexColors.push_back({
                pntsSrgbByteToLinear(c[0]),
                pntsSrgbByteToLinear(c[1]),
                pntsSrgbByteToLinear(c[2]),
                static_cast<float>(c[3]) / 255.0f});
        }
    }

    model->primitives.push_back(std::move(primitive));
    return model;
}

TileContentLoadResult decodePntsContent(const uint8_t* data,
                                        size_t size,
                                        const Mat4& baseTransform) {
    std::optional<PntsHeader> header = parsePntsHeader(data, size);
    if (!header) {
        return TileContentLoadResult::failed();
    }
    std::unique_ptr<GltfModel> model = parsePntsModel(data, *header);
    if (!model) {
        return TileContentLoadResult::failed();
    }
    if (model->primitives.empty()) {
        return TileContentLoadResult::empty();
    }
    TileContentLoadResult result =
        TileContentLoadResult::render(std::move(model));
    result.contentTransform = baseTransform;
    return result;
}

std::optional<CmptHeader> parseCmptHeader(const uint8_t* data, size_t size) {
    if (!data || size < 4 || readU32LE(data) != kCmptMagic) {
        return std::nullopt;
    }
    if (size < kCmptHeaderLength) {
        return std::nullopt;
    }

    CmptHeader header;
    header.version = readU32LE(data + 4);
    header.byteLength = readU32LE(data + 8);
    header.tilesLength = readU32LE(data + 12);

    if (header.version != 1 ||
        header.byteLength > size ||
        header.byteLength < kCmptHeaderLength) {
        return std::nullopt;
    }
    return header;
}

bool transformAlmostIdentity(const Mat4& transform) {
    const glm::dmat4 identity(1.0);
    const glm::dmat4& m = transform.raw();
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (std::abs(m[c][r] - identity[c][r]) > 1e-14) {
                return false;
            }
        }
    }
    return true;
}

bool transformStaticVertex(SurfaceVertex& vertex,
                           const glm::dmat4& transform,
                           const glm::dmat3& normalMatrix) {
    const glm::dvec4 position =
        transform * glm::dvec4(vertex.positionEcef.raw(), 1.0);
    if (!std::isfinite(position.w) || std::abs(position.w) <= 1e-14) {
        return false;
    }
    const glm::dvec3 transformedPosition =
        glm::dvec3(position) / position.w;
    if (!finiteVec3(transformedPosition)) {
        return false;
    }
    vertex.positionEcef = Vec3(transformedPosition);

    glm::dvec3 normal = normalMatrix * vertex.normalEcef.raw();
    const double lenSq = glm::dot(normal, normal);
    if (std::isfinite(lenSq) && lenSq > 0.0) {
        normal = glm::normalize(normal);
    } else {
        normal = Vec3::unitZ().raw();
    }
    if (!finiteVec3(normal)) {
        return false;
    }
    vertex.normalEcef = Vec3(normal);
    return true;
}

bool bakePrimitiveTransform(GltfPrimitive& primitive,
                            const Mat4& transform) {
    if (transformAlmostIdentity(transform)) {
        return true;
    }

    if (!primitive.instances.empty()) {
        for (GltfInstance& instance : primitive.instances) {
            instance.transform = transform * instance.transform;
        }
        return true;
    }

    const glm::dmat4& m = transform.raw();
    const glm::dmat3 linear(m);
    const double det = glm::determinant(linear);
    if (!std::isfinite(det) || std::abs(det) <= 1e-14) {
        return false;
    }
    const glm::dmat3 normalMatrix = glm::transpose(glm::inverse(linear));
    for (SurfaceVertex& vertex : primitive.vertices) {
        if (!transformStaticVertex(vertex, m, normalMatrix)) {
            return false;
        }
    }

    if (primitive.vertexTangents.size() == primitive.vertices.size()) {
        for (std::array<float, 4>& tangent : primitive.vertexTangents) {
            glm::dvec3 direction(tangent[0], tangent[1], tangent[2]);
            direction = linear * direction;
            const double lenSq = glm::dot(direction, direction);
            if (std::isfinite(lenSq) && lenSq > 0.0) {
                direction = glm::normalize(direction);
            } else {
                direction = glm::dvec3(0.0);
            }
            tangent[0] = static_cast<float>(direction.x);
            tangent[1] = static_cast<float>(direction.y);
            tangent[2] = static_cast<float>(direction.z);
        }
    }
    return true;
}

void offsetTextureBinding(std::optional<GltfTextureBinding>& binding,
                          size_t textureOffset) {
    if (binding) {
        binding->textureIndex += textureOffset;
    }
}

void offsetPrimitiveTextureBindings(GltfPrimitive& primitive,
                                    size_t textureOffset) {
    if (primitive.baseColorTextureIndex) {
        *primitive.baseColorTextureIndex += textureOffset;
    }
    offsetTextureBinding(primitive.baseColorTexture, textureOffset);
    offsetTextureBinding(primitive.metallicRoughnessTexture, textureOffset);
    offsetTextureBinding(primitive.anisotropyTexture, textureOffset);
    offsetTextureBinding(primitive.specularTexture, textureOffset);
    offsetTextureBinding(primitive.specularColorTexture, textureOffset);
    offsetTextureBinding(primitive.specularGlossinessTexture, textureOffset);
    offsetTextureBinding(primitive.transmissionTexture, textureOffset);
    offsetTextureBinding(primitive.clearcoatTexture, textureOffset);
    offsetTextureBinding(primitive.clearcoatRoughnessTexture, textureOffset);
    offsetTextureBinding(primitive.clearcoatNormalTexture, textureOffset);
    offsetTextureBinding(primitive.sheenColorTexture, textureOffset);
    offsetTextureBinding(primitive.sheenRoughnessTexture, textureOffset);
    offsetTextureBinding(primitive.normalTexture, textureOffset);
    offsetTextureBinding(primitive.occlusionTexture, textureOffset);
    offsetTextureBinding(primitive.emissiveTexture, textureOffset);
}

bool appendCompositeModel(GltfModel& composite,
                          std::unique_ptr<GltfModel> model,
                          const Mat4& innerTransform) {
    if (!model) {
        return false;
    }
    if (model->hasRuntimeAnimation()) {
        return false;
    }

    const size_t textureOffset = composite.textures.size();
    composite.textures.insert(
        composite.textures.end(),
        std::make_move_iterator(model->textures.begin()),
        std::make_move_iterator(model->textures.end()));

    for (GltfPrimitive& primitive : model->primitives) {
        offsetPrimitiveTextureBindings(primitive, textureOffset);
        if (!bakePrimitiveTransform(primitive, innerTransform)) {
            return false;
        }
        composite.primitives.push_back(std::move(primitive));
    }
    return true;
}

TileContentLoadResult decodeCmptContent(
    const uint8_t* data,
    size_t size,
    const Mat4& baseTransform,
    const Mat4& gltfUpAxisTransform,
    const std::string& contentUrl,
    const GltfParser::ExternalResourceResolver& resolver,
    const GltfParser::ImageDecoder& imageDecoder) {
    std::optional<CmptHeader> header = parseCmptHeader(data, size);
    if (!header) {
        return TileContentLoadResult::failed();
    }

    size_t offset = kCmptHeaderLength;
    std::vector<TileContentLoadResult> renderResults;
    renderResults.reserve(header->tilesLength);
    for (uint32_t i = 0; i < header->tilesLength; ++i) {
        if (offset + kCmptInnerHeaderLength > header->byteLength) {
            return TileContentLoadResult::failed();
        }
        const uint32_t innerByteLength = readU32LE(data + offset + 8);
        if (innerByteLength < kCmptInnerHeaderLength ||
            static_cast<uint64_t>(offset) + innerByteLength >
                header->byteLength) {
            return TileContentLoadResult::failed();
        }

        TileContentLoadResult inner = decodeGltfLikeContent(
            data + offset,
            innerByteLength,
            Mat4::identity(),
            gltfUpAxisTransform,
            contentUrl,
            resolver,
            imageDecoder);
        if (inner.status == TileContentLoadStatus::Empty) {
            offset += innerByteLength;
            continue;
        }
        if (inner.status != TileContentLoadStatus::Render ||
            !inner.gltfModel ||
            inner.gltfModel->primitives.empty()) {
            return TileContentLoadResult::failed();
        }
        renderResults.push_back(std::move(inner));
        offset += innerByteLength;
    }

    if (offset != header->byteLength) {
        return TileContentLoadResult::failed();
    }
    if (renderResults.empty()) {
        return TileContentLoadResult::empty();
    }
    if (renderResults.size() == 1) {
        TileContentLoadResult result = std::move(renderResults.front());
        result.contentTransform = baseTransform * result.contentTransform;
        return result;
    }

    auto composite = std::make_unique<GltfModel>();
    for (TileContentLoadResult& inner : renderResults) {
        if (!appendCompositeModel(
                *composite,
                std::move(inner.gltfModel),
                inner.contentTransform)) {
            return TileContentLoadResult::failed();
        }
    }
    if (composite->primitives.empty()) {
        return TileContentLoadResult::empty();
    }

    TileContentLoadResult result =
        TileContentLoadResult::render(std::move(composite));
    result.contentTransform = baseTransform;
    return result;
}

TileContentLoadResult decodeGltfLikeContent(
    const uint8_t* data,
    size_t size,
    const Mat4& baseTransform,
    const Mat4& gltfUpAxisTransform,
    const std::string& contentUrl,
    const GltfParser::ExternalResourceResolver& resolver =
        GltfParser::ExternalResourceResolver{},
    const GltfParser::ImageDecoder& imageDecoder =
        GltfParser::ImageDecoder{}) {
    if (data && size >= 4 && readU32LE(data) == kI3dmMagic) {
        return decodeI3dmContent(
            data,
            size,
            baseTransform,
            gltfUpAxisTransform,
            contentUrl,
            resolver,
            imageDecoder);
    }
    if (data && size >= 4 && readU32LE(data) == kPntsMagic) {
        return decodePntsContent(data, size, baseTransform);
    }
    if (data && size >= 4 && readU32LE(data) == kCmptMagic) {
        return decodeCmptContent(
            data,
            size,
            baseTransform,
            gltfUpAxisTransform,
            contentUrl,
            resolver,
            imageDecoder);
    }

    Mat4 contentTransform = baseTransform;
    const B3dmExtractResult b3dm = extractB3dmGlb(data, size);
    if (b3dm.isB3dm) {
        if (!b3dm.valid) {
            return TileContentLoadResult::failed();
        }
        data = b3dm.glbData;
        size = b3dm.glbSize;
        contentTransform = contentTransform * b3dm.rtcTransform;
    }
    contentTransform = contentTransform * gltfUpAxisTransform;

    std::unique_ptr<GltfModel> model =
        GltfParser::parse(data, size, resolver, imageDecoder);
    if (!model || model->primitives.empty()) {
        return TileContentLoadResult::failed();
    }
    TileContentLoadResult result =
        TileContentLoadResult::render(std::move(model));
    result.contentTransform = contentTransform;
    return result;
}

} // namespace

SingleGltfContentProvider::SingleGltfContentProvider(
    TileKey contentKey,
    std::string url,
    std::string name)
    : contentKey_(std::move(contentKey)),
      url_(std::move(url)),
      name_(std::move(name)) {}

SingleGltfContentProvider::SingleGltfContentProvider(
    TileKey contentKey,
    std::vector<uint8_t> bytes,
    std::string name)
    : contentKey_(std::move(contentKey)),
      name_(std::move(name)),
      bytes_(std::move(bytes)) {}

std::string SingleGltfContentProvider::id() const {
    std::ostringstream oss;
    oss << "gltf-content-" << std::hash<std::string>{}(url_ + name_);
    return oss.str();
}

bool SingleGltfContentProvider::supportsTile(const TileKey& key) const {
    return key == contentKey_;
}

void SingleGltfContentProvider::requestTileContent(
    const TileKey& key,
    CancellationToken token,
    ContentCallback callback) {
    if (!supportsTile(key)) {
        callback(key, TileContentLoadResult::empty());
        return;
    }

    AsyncSystem::pool().enqueue(
        [this,
         key,
         token = std::move(token),
         callback = std::move(callback)]() mutable {
            if (token.isCancelled()) {
                callback(key, TileContentLoadResult::cancelled());
                return;
            }

            std::vector<uint8_t> body = bytes_;
            if (body.empty() && !url_.empty()) {
                body = httpGet(url_);
            }

            if (token.isCancelled()) {
                callback(key, TileContentLoadResult::cancelled());
                return;
            }
            if (body.empty()) {
                callback(key, TileContentLoadResult::retryLater());
                return;
            }

            callback(key, decodeContent(body.data(), body.size()));
        });
}

TileContentLoadResult SingleGltfContentProvider::decodeContent(
    const uint8_t* data,
    size_t size) {
    GltfParser::ExternalResourceResolver resolver;
    if (!url_.empty()) {
        resolver = [this](const std::string& uri) {
            return httpGet(resolveContentUrl(url_, uri, false));
        };
    }
    return decodeGltfLikeContent(
        data,
        size,
        contentTransform_,
        Mat4::identity(),
        url_,
        resolver,
        makeImageDecoder(platformBridge_));
}

void SingleGltfContentProvider::setEastNorthUpPlacementDegrees(
    double longitudeDegrees,
    double latitudeDegrees,
    double heightMeters,
    double uniformScale) {
    const Cartographic cartographic =
        Cartographic::fromDegrees(
            longitudeDegrees,
            latitudeDegrees,
            heightMeters);
    const Vec3 origin =
        Ellipsoid::WGS84().cartographicToCartesian(cartographic);
    const Mat4 enu = Transforms::eastNorthUpToFixedFrame(origin);
    contentTransform_ =
        enu * Mat4::scale(Vec3(uniformScale, uniformScale, uniformScale));
}

std::vector<uint8_t> SingleGltfContentProvider::httpGet(
    const std::string& url) const {
    auto cached = HttpCache::shared().get(url);
    if (!cached.empty()) return cached;

    constexpr const char* kFilePrefix = "file://";
    if (url.rfind(kFilePrefix, 0) == 0) {
        std::ifstream in(url.substr(std::strlen(kFilePrefix)), std::ios::binary);
        if (!in) return {};
        std::vector<uint8_t> data{
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
        HttpCache::shared().put(url, data);
        return data;
    }

    if (platformBridge_) {
        std::vector<uint8_t> result;
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;

        platformBridge_->get(url, [&](int code, std::vector<uint8_t> body) {
            if (code == 200) result = std::move(body);
            {
                std::lock_guard<std::mutex> lock(mutex);
                done = true;
            }
            cv.notify_one();
        });

        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait_for(lock, std::chrono::seconds(20), [&] {
                return done;
            });
        }
        if (!result.empty()) HttpCache::shared().put(url, result);
        return result;
    }

#if EARTH_ENGINE_HAS_LIBCURL
    ensureCurl();
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::vector<uint8_t> body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "earth-md/0.1");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);

    const CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    std::vector<uint8_t> result =
        (res == CURLE_OK && httpCode == 200) ? std::move(body)
                                             : std::vector<uint8_t>{};
    if (!result.empty()) HttpCache::shared().put(url, result);
    return result;
#else
    (void)url;
    return {};
#endif
}

TilesetJsonContentProvider::TilesetJsonContentProvider(
    std::string tilesetJsonUrl,
    std::vector<uint8_t> tilesetJsonBytes,
    std::string name)
    : tilesetJsonUrl_(std::move(tilesetJsonUrl)),
      name_(std::move(name)) {
    std::ostringstream oss;
    oss << "3dtiles-json-" << std::hash<std::string>{}(
        tilesetJsonUrl_ + name_);
    schemeId_ = oss.str();

    if (tilesetJsonBytes.empty() && !tilesetJsonUrl_.empty()) {
        tilesetJsonBytes = httpGet(tilesetJsonUrl_);
    }
    if (!tilesetJsonBytes.empty()) {
        valid_ = parseTilesetJson(tilesetJsonBytes.data(),
                                  tilesetJsonBytes.size(),
                                  tilesetJsonUrl_,
                                  Mat4::identity(),
                                  TileRefine::Replace,
                                  10000000.0,
                                  std::nullopt,
                                  true);
    }
}

std::string TilesetJsonContentProvider::id() const {
    return schemeId_;
}

bool TilesetJsonContentProvider::valid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return valid_;
}

bool TilesetJsonContentProvider::supportsTile(const TileKey& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.find(contentCacheKey(key)) != records_.end();
}

std::vector<TileKey> TilesetJsonContentProvider::rootTiles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rootKeys_;
}

std::optional<TilesetContentTileMetadata>
TilesetJsonContentProvider::tileMetadata(const TileKey& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(contentCacheKey(key));
    if (it == records_.end()) return std::nullopt;
    return it->second.metadata;
}

std::vector<TileKey> TilesetJsonContentProvider::childTiles(
    const TileKey& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(contentCacheKey(key));
    if (it == records_.end()) return {};
    return it->second.metadata.childKeys;
}

void TilesetJsonContentProvider::requestTileContent(
    const TileKey& key,
    CancellationToken token,
    ContentCallback callback) {
    TileRecord record;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = records_.find(contentCacheKey(key));
        if (it == records_.end()) {
            callback(key, TileContentLoadResult::failed());
            return;
        }
        record = it->second;
    }

    if (record.contentKind == TileRecordContentKind::Empty) {
        callback(key, TileContentLoadResult::empty());
        return;
    }
    if (record.contentKind == TileRecordContentKind::External) {
        callback(key, TileContentLoadResult::external());
        return;
    }
    if (record.contentKind == TileRecordContentKind::Unsupported) {
        callback(key, TileContentLoadResult::failed());
        return;
    }

    AsyncSystem::pool().enqueue(
        [this,
         key,
         record = std::move(record),
         token = std::move(token),
         callback = std::move(callback)]() mutable {
            if (token.isCancelled()) {
                callback(key, TileContentLoadResult::cancelled());
                return;
            }

            const std::vector<uint8_t> body = httpGet(record.resolvedContentUrl);
            if (token.isCancelled()) {
                callback(key, TileContentLoadResult::cancelled());
                return;
            }
            if (body.empty()) {
                callback(key, TileContentLoadResult::retryLater());
                return;
            }

            if (!urlLooksLikeGltf(record.resolvedContentUrl) &&
                (urlLooksLikeJson(record.resolvedContentUrl) ||
                 bytesLookLikeJson(body.data(), body.size()))) {
                const bool parsed = parseTilesetJson(
                    body.data(),
                    body.size(),
                    record.resolvedContentUrl,
                    record.metadata.transform,
                    record.metadata.refine,
                    record.metadata.geometricError,
                    record.metadata.key,
                    false);
                callback(key,
                         parsed ? TileContentLoadResult::external()
                                : TileContentLoadResult::failed());
                return;
            }

            callback(key,
                     decodeRenderableContent(
                         body.data(),
                         body.size(),
                         record.metadata.transform,
                         record.gltfUpAxisTransform,
                         record.resolvedContentUrl));
        });
}

TileContentLoadResult TilesetJsonContentProvider::decodeContent(
    const uint8_t* data,
    size_t size) {
    return decodeRenderableContent(
        data,
        size,
        Mat4::identity(),
        Mat4::identity(),
        tilesetJsonUrl_);
}

TileContentLoadResult TilesetJsonContentProvider::decodeRenderableContent(
    const uint8_t* data,
    size_t size,
    const Mat4& transform,
    const Mat4& gltfUpAxisTransform,
    const std::string& contentUrl) {
    GltfParser::ExternalResourceResolver resolver;
    if (!contentUrl.empty()) {
        resolver = [this, contentUrl](const std::string& uri) {
            return httpGet(resolveContentUrl(contentUrl, uri, false));
        };
    }
    return decodeGltfLikeContent(
        data,
        size,
        transform,
        gltfUpAxisTransform,
        contentUrl,
        resolver,
        makeImageDecoder(platformBridge_));
}

bool TilesetJsonContentProvider::parseTilesetJson(
    const uint8_t* data,
    size_t size,
    const std::string& baseUrl,
    const Mat4& parentTransform,
    TileRefine parentRefine,
    double parentGeometricError,
    const std::optional<TileKey>& externalParentKey,
    bool wrapRoot) {
    if (!data || size == 0) return false;

    auto parsed = nlohmann::json::parse(
        reinterpret_cast<const char*>(data),
        reinterpret_cast<const char*>(data) + size,
        nullptr,
        false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return false;
    }
    auto rootIt = parsed.find("root");
    if (rootIt == parsed.end() || !rootIt->is_object()) {
        return false;
    }
    if (hasUnsupportedDeclaredExtensionsOnObject(parsed, true) ||
        hasUnsupportedDirectExtensionsObject(parsed, true)) {
        return false;
    }
    const Mat4 gltfUpAxisTransform = parseGltfUpAxisTransform(parsed);

    std::lock_guard<std::mutex> lock(mutex_);

    std::function<std::optional<TileKey>(
        const nlohmann::json&,
        const Mat4&,
        TileRefine,
        double,
        const std::optional<TileKey>&,
        int)> parseTile;

    parseTile = [&](const nlohmann::json& tileJson,
                    const Mat4& inheritedTransform,
                    TileRefine inheritedRefine,
                    double inheritedGeometricError,
                    const std::optional<TileKey>& parentKey,
                    int depth) -> std::optional<TileKey> {
        if (!tileJson.is_object()) return std::nullopt;
        const bool unsupportedMultipleContents =
            hasUnsupportedMultipleContents(tileJson);
        const bool unsupportedImplicitTiling =
            hasUnsupportedImplicitTiling(tileJson);
        const bool unsupportedRequiredExtensions =
            hasUnsupportedTileRequiredExtensions(tileJson);

        Mat4 tileTransform = inheritedTransform;
        auto transformIt = tileJson.find("transform");
        if (transformIt != tileJson.end()) {
            std::optional<Mat4> localTransform = parseTransform(*transformIt);
            if (!localTransform) return std::nullopt;
            tileTransform = inheritedTransform * *localTransform;
        }

        auto bvIt = tileJson.find("boundingVolume");
        if (bvIt == tileJson.end()) return std::nullopt;
        std::optional<TileBoundingVolume> localVolume =
            parseBoundingVolumeJson(*bvIt);
        if (!localVolume) return std::nullopt;
        std::optional<TileBoundingVolume> tileVolume =
            transformBoundingVolume(tileTransform, *localVolume);

        double geometricError = inheritedGeometricError * 0.5;
        auto geometricErrorIt = tileJson.find("geometricError");
        if (geometricErrorIt != tileJson.end() &&
            geometricErrorIt->is_number()) {
            geometricError = geometricErrorIt->get<double>();
        }
        geometricError *= maxScaleComponent(tileTransform);

        TileRefine refine = inheritedRefine;
        auto refineIt = tileJson.find("refine");
        if (refineIt != tileJson.end() && refineIt->is_string()) {
            const std::string refineUpper =
                toLowerAscii(refineIt->get<std::string>());
            if (refineUpper == "replace") {
                refine = TileRefine::Replace;
            } else if (refineUpper == "add") {
                refine = TileRefine::Add;
            }
        }

        TileKey key{schemeId_, depth, nextTileOrdinal_++, 0};
        TileRecord record;
        record.metadata.key = key;
        record.metadata.parentKey = parentKey;
        record.metadata.bounds = boundsFromVolumeOrWorld(tileVolume);
        record.metadata.hasExplicitBounds = true;
        record.metadata.boundingVolume = tileVolume;
        record.metadata.viewerRequestVolume =
            parseViewerRequestVolume(tileJson, tileTransform);
        record.metadata.contentBoundingVolume =
            parseContentBoundingVolume(tileJson, tileTransform);
        record.metadata.transform = tileTransform;
        record.metadata.geometricError = geometricError;
        record.metadata.refine = refine;
        record.gltfUpAxisTransform = gltfUpAxisTransform;

        std::optional<std::string> contentUri = parseContentUri(tileJson);
        if (unsupportedMultipleContents ||
            unsupportedImplicitTiling ||
            unsupportedRequiredExtensions) {
            record.contentKind = TileRecordContentKind::Unsupported;
        } else if (!contentUri || contentUri->empty()) {
            record.contentKind = TileRecordContentKind::Empty;
        } else {
            record.contentKind = TileRecordContentKind::Uri;
            record.contentUri = *contentUri;
            record.resolvedContentUrl =
                resolveContentUrl(baseUrl, *contentUri, true);
        }

        auto childrenIt = tileJson.find("children");
        if (childrenIt != tileJson.end() && childrenIt->is_array()) {
            for (const auto& childJson : *childrenIt) {
                std::optional<TileKey> childKey = parseTile(
                    childJson,
                    tileTransform,
                    refine,
                    geometricError,
                    key,
                    depth + 1);
                if (childKey) {
                    record.metadata.childKeys.push_back(*childKey);
                }
            }
        }

        records_[contentCacheKey(key)] = std::move(record);
        return key;
    };

    if (wrapRoot) {
        TileKey wrapperKey{schemeId_, 0, nextTileOrdinal_++, 0};
        std::optional<TileKey> childKey = parseTile(
            *rootIt,
            parentTransform,
            parentRefine,
            parentGeometricError,
            wrapperKey,
            1);
        if (!childKey) return false;

        auto childIt = records_.find(contentCacheKey(*childKey));
        if (childIt == records_.end()) return false;

        TileRecord wrapper;
        wrapper.metadata = childIt->second.metadata;
        wrapper.metadata.key = wrapperKey;
        wrapper.metadata.parentKey = std::nullopt;
        wrapper.metadata.childKeys = {*childKey};
        wrapper.metadata.unconditionallyRefine = true;
        wrapper.contentKind = TileRecordContentKind::External;
        records_[contentCacheKey(wrapperKey)] = std::move(wrapper);
        rootKeys_.clear();
        rootKeys_.push_back(wrapperKey);
        return true;
    }

    const int rootDepth = externalParentKey ? externalParentKey->z + 1 : 0;
    std::optional<TileKey> rootKey = parseTile(
        *rootIt,
        parentTransform,
        parentRefine,
        parentGeometricError,
        externalParentKey,
        rootDepth);
    if (!rootKey) return false;

    if (externalParentKey) {
        auto parentIt = records_.find(contentCacheKey(*externalParentKey));
        if (parentIt != records_.end()) {
            parentIt->second.metadata.unconditionallyRefine = true;
            auto& children = parentIt->second.metadata.childKeys;
            if (std::find(children.begin(), children.end(), *rootKey) ==
                children.end()) {
                children.push_back(*rootKey);
            }
        }
    } else {
        rootKeys_.clear();
        rootKeys_.push_back(*rootKey);
    }
    return true;
}

std::vector<uint8_t> TilesetJsonContentProvider::httpGet(
    const std::string& url) const {
    auto cached = HttpCache::shared().get(url);
    if (!cached.empty()) return cached;

    constexpr const char* kFilePrefix = "file://";
    if (url.rfind(kFilePrefix, 0) == 0) {
        std::ifstream in(url.substr(std::strlen(kFilePrefix)), std::ios::binary);
        if (!in) return {};
        std::vector<uint8_t> data{
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
        HttpCache::shared().put(url, data);
        return data;
    }

    if (platformBridge_) {
        std::vector<uint8_t> result;
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;

        platformBridge_->get(url, [&](int code, std::vector<uint8_t> body) {
            if (code == 200) result = std::move(body);
            {
                std::lock_guard<std::mutex> lock(mutex);
                done = true;
            }
            cv.notify_one();
        });

        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait_for(lock, std::chrono::seconds(20), [&] {
                return done;
            });
        }
        if (!result.empty()) HttpCache::shared().put(url, result);
        return result;
    }

#if EARTH_ENGINE_HAS_LIBCURL
    ensureCurl();
    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::vector<uint8_t> body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "earth-md/0.1");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);

    const CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    std::vector<uint8_t> result =
        (res == CURLE_OK && httpCode == 200) ? std::move(body)
                                             : std::vector<uint8_t>{};
    if (!result.empty()) HttpCache::shared().put(url, result);
    return result;
#else
    (void)url;
    return {};
#endif
}

} // namespace earth_engine
