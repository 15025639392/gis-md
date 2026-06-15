#include "QuantizedMeshTerrainProvider.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include "../core/async/AsyncSystem.h"
#include "../core/cache/HttpCache.h"
#include "../platform/bridge/PlatformBridge.h"
#include "../terrain/QuantizedMeshParser.h"
#include <nlohmann/json.hpp>

#ifndef EARTH_ENGINE_HAS_LIBCURL
#if !defined(ANDROID) && __has_include(<curl/curl.h>)
#include <curl/curl.h>
#define EARTH_ENGINE_HAS_LIBCURL 1
#else
#define EARTH_ENGINE_HAS_LIBCURL 0
#endif
#endif

#include <sstream>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <vector>
#include <unordered_set>
#include <functional>
#include <limits>

namespace earth_engine {

// ============================================================
// libcurl helpers
// ============================================================

namespace {
#if EARTH_ENGINE_HAS_LIBCURL
static std::once_flag gCurlOnce;
static void ensureCurl() {
    std::call_once(gCurlOnce, []{ curl_global_init(CURL_GLOBAL_DEFAULT); });
}
static size_t curlWrite(void* p, size_t s, size_t n, void* u) {
    auto* b = static_cast<std::vector<uint8_t>*>(u);
    size_t t = s * n;
    b->insert(b->end(), static_cast<const uint8_t*>(p),
              static_cast<const uint8_t*>(p) + t);
    return t;
}
#endif
} // namespace

QuantizedMeshTerrainProvider::QuantizedMeshTerrainProvider(
    std::string urlTemplate, std::string attribution)
    : urlTemplate_(std::move(urlTemplate)),
      attribution_(std::move(attribution)) {}

QuantizedMeshTerrainProvider::~QuantizedMeshTerrainProvider() = default;

void QuantizedMeshTerrainProvider::setPlatformBridge(PlatformBridge* bridge) {
    platformBridge_ = bridge;
}

void QuantizedMeshTerrainProvider::setZoomRange(int minZ, int maxZ) {
    minZoom_ = minZ;
    maxZoom_ = maxZ;
}

namespace {

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
        if (c == ':') {
            return i > 0;
        }
        if (c == '/' || c == '?' || c == '#') {
            return false;
        }
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
            const size_t authorityEnd = url.find_first_of("/?#", authorityStart);
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
            // Skip.
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
    if (result.empty() && absolute) {
        result = "/";
    }
    return result;
}

std::string baseDirectoryPath(const std::string& basePath) {
    const size_t slash = basePath.find_last_of('/');
    if (slash == std::string::npos) return "";
    return basePath.substr(0, slash + 1);
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

std::string resolveTerrainTemplate(const std::string& layerJsonUrl,
                                   const std::string& tileTemplate) {
    // cesium-native LayerJsonTerrainLoader::resolveTileUrl uses
    // CesiumUtility::Uri(layer.baseUrl, template, true), so relative templates
    // resolve against the layer.json URL and inherit base query parameters.
    ParsedUrl base = parseUrl(layerJsonUrl);
    ParsedUrl relative = parseUrl(tileTemplate);
    ParsedUrl resolved = relative;

    if (tileTemplate.rfind("//", 0) == 0 && !relative.hasScheme) {
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

    const std::string mergedQuery =
        mergeBaseQuery(resolved.hasQuery ? resolved.query : std::string(),
                       base.hasQuery ? base.query : std::string());
    resolved.hasQuery = !mergedQuery.empty();
    resolved.query = mergedQuery;
    return composeUrl(resolved);
}

std::string resolveParentLayerJsonUrl(const std::string& layerJsonUrl,
                                      const std::string& parentUrl) {
    ParsedUrl base = parseUrl(layerJsonUrl);
    ParsedUrl relative = parseUrl(parentUrl);
    ParsedUrl resolved = relative;

    if (parentUrl.rfind("//", 0) == 0 && !relative.hasScheme) {
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

    resolved.hasQuery = relative.hasQuery;
    resolved.query = relative.query;
    std::string url = composeUrl(resolved);
    if (url.empty() || url.back() != '/') {
        url += "/";
    }
    url += "layer.json";
    return url;
}

std::string substituteTemplateParameters(
    const std::string& templateUrl,
    const std::function<std::string(const std::string&)>& substitute) {
    std::string result;
    size_t start = 0;
    while (true) {
        const size_t open = templateUrl.find('{', start);
        if (open == std::string::npos) {
            result.append(templateUrl, start, templateUrl.size() - start);
            break;
        }

        result.append(templateUrl, start, open - start);
        const size_t close = templateUrl.find('}', open + 1);
        if (close == std::string::npos) {
            result.append(templateUrl, open, templateUrl.size() - open);
            break;
        }

        result += substitute(templateUrl.substr(open + 1, close - open - 1));
        start = close + 1;
    }
    return result;
}

uint64_t mortonEncode2D(uint32_t x, uint32_t y) {
    uint64_t result = 0;
    for (int i = 0; i < 32; ++i) {
        result |= (uint64_t)((x >> i) & 1) << (2 * i);
        result |= (uint64_t)((y >> i) & 1) << (2 * i + 1);
    }
    return result;
}

bool isGeographicTmsTileInRange(const TileKey& key) {
    if (key.schemeId != "Geographic-TMS" || key.z < 0 ||
        key.x < 0 || key.y < 0) {
        return false;
    }

    const double xTiles = std::ldexp(1.0, key.z + 1);
    const double yTiles = std::ldexp(1.0, key.z);
    return static_cast<double>(key.x) < xTiles &&
           static_cast<double>(key.y) < yTiles;
}

std::vector<std::string> jsonStringArray(
    const nlohmann::json& j,
    const char* name) {
    std::vector<std::string> result;
    if (!j.contains(name) || !j[name].is_array()) return result;
    for (const auto& item : j[name]) {
        if (item.is_string()) {
            result.push_back(item.get<std::string>());
        }
    }
    return result;
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

std::string createExtensionsQueryParameter(
    const std::vector<std::string>& knownExtensions,
    const std::vector<std::string>& extensions) {
    std::string result;
    for (const std::string& extension : knownExtensions) {
        if (std::find(extensions.begin(), extensions.end(), extension) ==
            extensions.end()) {
            continue;
        }
        if (!result.empty()) result += "-";
        result += extension;
    }
    return result;
}

void setQueryParameter(std::string& url,
                       const std::string& name,
                       const std::string& value) {
    if (value.empty()) return;
    const std::string assignment = name + "=" + value;
    const size_t queryStart = url.find('?');
    if (queryStart == std::string::npos) {
        url += "?" + assignment;
        return;
    }

    const std::string needle = name + "=";
    size_t pos = url.find(needle, queryStart + 1);
    if (pos == std::string::npos) {
        url += "&" + assignment;
        return;
    }

    size_t valueEnd = url.find('&', pos);
    if (valueEnd == std::string::npos) valueEnd = url.size();
    url.replace(pos, valueEnd - pos, assignment);
}

} // namespace

bool QuantizedMeshTerrainProvider::configureFromLayerJsonUrl(
    const std::string& layerJsonUrl) {
    auto bytes = httpGet(layerJsonUrl);
    if (bytes.empty()) return false;
    std::string body(bytes.begin(), bytes.end());
    return configureFromLayerJson(body, layerJsonUrl);
}

bool QuantizedMeshTerrainProvider::configureFromLayerJson(
    const std::string& layerJson,
    const std::string& layerJsonUrl) {
    try {
        auto j = nlohmann::json::parse(layerJson);
        if (j.value("format", "") != "quantized-mesh-1.0") return false;
        if (j.value("projection", "EPSG:4326") != "EPSG:4326") return false;
        if (j.value("scheme", "tms") != "tms") return false;

        layers_.clear();
        if (!appendLayerFromJson(j, layerJsonUrl)) {
            return false;
        }
        appendParentLayers(j, layerJsonUrl);
        syncLegacyFieldsFromPrimaryLayer();
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "QMTerrain",
            "layer.json OK: url=%s z=%d-%d layers=%zu ranges=%zu",
            urlTemplate_.c_str(), minZoom_, maxZoom_, layers_.size(),
            availabilityRanges_.size());
#endif
        return !urlTemplate_.empty();
    } catch (...) {
        return false;
    }
}

bool QuantizedMeshTerrainProvider::appendLayerFromJson(
    const nlohmann::json& j,
    const std::string& layerJsonUrl) {
    if (!j.contains("tiles") || !j["tiles"].is_array() || j["tiles"].empty() ||
        !j["tiles"][0].is_string()) {
        return false;
    }

    LayerConfig layer;
    layer.urlTemplate =
        resolveTerrainTemplate(layerJsonUrl, j["tiles"][0].get<std::string>());
    layer.layerJsonUrl = layerJsonUrl;
    // cesium-native LayerJsonTerrainLoader reads layer.json maxzoom for
    // availability storage/subtree bookkeeping, but minzoom/maxzoom do not
    // directly gate tile availability. Availability rectangles and loaded
    // metadata subtrees determine whether tiles are requestable.
    layer.minZoom = 0;
    layer.maxZoom = j.value("maxzoom", maxZoom_);
    layer.version = j.value("version", std::string());
    layer.extensionsToRequest = createExtensionsQueryParameter(
        {"octvertexnormals", "metadata"},
        jsonStringArray(j, "extensions"));

    const bool hasMetadataAvailability =
        j.contains("metadataAvailability") &&
        j["metadataAvailability"].is_number_integer();
    layer.availabilityLevels = hasMetadataAvailability
        ? j["metadataAvailability"].get<int>()
        : -1;
    // cesium-native always constructs a QuadtreeRectangleAvailability for
    // layer.json terrain. If `available` is absent or empty, that empty table
    // still makes only level-0 roots available by default.
    layer.hasAvailability = true;

    if (layer.availabilityLevels > 0) {
        const int subtreeCount =
            (layer.maxZoom + layer.availabilityLevels - 1) /
            layer.availabilityLevels;
        if (subtreeCount > 0) {
            layer.loadedSubtrees.resize(static_cast<size_t>(subtreeCount));
        }
    }

    // cesium-native LayerJsonTerrainLoader::loadLayersRecursive:
    // when metadataAvailability is present, layer.json `available` is not
    // loaded into contentAvailability. Deeper availability must come from
    // quantized-mesh metadata subtrees.
    if (!hasMetadataAvailability &&
        j.contains("available") && j["available"].is_array()) {
        layer.availabilityRanges.resize(j["available"].size());
        for (size_t level = 0; level < j["available"].size(); ++level) {
            const auto& levelRanges = j["available"][level];
            if (!levelRanges.is_array()) continue;
            for (const auto& range : levelRanges) {
                if (!range.is_object()) continue;
                layer.availabilityRanges[level].push_back({
                    jsonUint32OrDefault(range, "startX"),
                    jsonUint32OrDefault(range, "startY"),
                    jsonUint32OrDefault(range, "endX"),
                    jsonUint32OrDefault(range, "endY")
                });
            }
        }
    }

    if (layer.urlTemplate.empty()) {
        return false;
    }
    layers_.push_back(std::move(layer));
    return true;
}

bool QuantizedMeshTerrainProvider::appendParentLayers(
    const nlohmann::json& j,
    const std::string& layerJsonUrl) {
    std::string parentUrl = j.value("parentUrl", std::string());
    if (parentUrl.empty()) {
        return true;
    }

    std::string resolvedUrl = resolveParentLayerJsonUrl(layerJsonUrl, parentUrl);
    auto bytes = httpGet(resolvedUrl);
    if (bytes.empty()) {
        return false;
    }

    try {
        const std::string body(bytes.begin(), bytes.end());
        auto parent = nlohmann::json::parse(body);
        if (parent.value("format", "") != "quantized-mesh-1.0") return false;
        if (parent.value("projection", "EPSG:4326") != "EPSG:4326") return false;
        if (parent.value("scheme", "tms") != "tms") return false;
        if (!appendLayerFromJson(parent, resolvedUrl)) {
            return false;
        }
        appendParentLayers(parent, resolvedUrl);
        return true;
    } catch (...) {
        return false;
    }
}

void QuantizedMeshTerrainProvider::syncLegacyFieldsFromPrimaryLayer() {
    if (layers_.empty()) return;

    const LayerConfig& primary = layers_.front();
    urlTemplate_ = primary.urlTemplate;
    layerJsonUrl_ = primary.layerJsonUrl;
    version_ = primary.version;
    extensionsToRequest_ = primary.extensionsToRequest;
    availabilityRanges_ = primary.availabilityRanges;
    loadedSubtrees_ = primary.loadedSubtrees;
    hasAvailability_ = primary.hasAvailability;
    availabilityLevels_ = primary.availabilityLevels;
    minZoom_ = primary.minZoom;
    maxZoom_ = primary.maxZoom;
    for (const LayerConfig& layer : layers_) {
        minZoom_ = std::min(minZoom_, layer.minZoom);
        maxZoom_ = std::max(maxZoom_, layer.maxZoom);
    }
}

bool QuantizedMeshTerrainProvider::supportsTile(const TileKey& key) const {
    return availabilityState(key) == TileAvailabilityState::Available;
}

uint32_t QuantizedMeshTerrainProvider::maximumAvailableLevelAtTileCenter(
    const LayerConfig& layer,
    const TileKey& key) const {
    // cesium-native QuadtreeRectangleAvailability::isTileAvailable:
    // query the maximum available level at the center of this tile. Empty
    // availability has maxLevel=0, so root tiles remain requestable but deeper
    // tiles wait for metadata.
    uint32_t maxLevel = 0;
    if (layer.availabilityRanges.empty()) return maxLevel;

    const double keyXTiles = std::ldexp(1.0, key.z + 1);
    const double keyYTiles = std::ldexp(1.0, key.z);
    const double centerX = (static_cast<double>(key.x) + 0.5) / keyXTiles;
    const double centerY = (static_cast<double>(key.y) + 0.5) / keyYTiles;

    for (size_t levelIndex = 0; levelIndex < layer.availabilityRanges.size(); ++levelIndex) {
        const auto& ranges = layer.availabilityRanges[levelIndex];
        if (ranges.empty()) continue;

        const int level = static_cast<int>(levelIndex);
        const double xTiles = std::ldexp(1.0, level + 1);
        const double yTiles = std::ldexp(1.0, level);
        for (const auto& range : ranges) {
            // cesium-native QuadtreeRectangleAvailability stores each
            // available tile range as a projected rectangle and uses
            // Rectangle::contains, whose min/max edges are both inclusive.
            // A coarser tile center can lie exactly on a deeper range edge;
            // floor(center * tileCount) would incorrectly pick only one side.
            const double minX = static_cast<double>(range[0]) / xTiles;
            const double maxX = static_cast<double>(range[2] + 1) / xTiles;
            const double minY = static_cast<double>(range[1]) / yTiles;
            const double maxY = static_cast<double>(range[3] + 1) / yTiles;
            if (centerX >= minX && centerX <= maxX &&
                centerY >= minY && centerY <= maxY) {
                maxLevel = std::max(maxLevel, static_cast<uint32_t>(level));
                break;
            }
        }
    }
    return maxLevel;
}

bool QuantizedMeshTerrainProvider::isSubtreeLoadedInLayer(
    const LayerConfig& layer,
    int subtreeLevel,
    uint64_t mortonIndex) const {
    if (subtreeLevel < 0) {
        return false;
    }
    if (layer.availabilityLevels > 0 &&
        static_cast<size_t>(subtreeLevel) >= layer.loadedSubtrees.size()) {
        return true;
    }
    if (static_cast<size_t>(subtreeLevel) >= layer.loadedSubtrees.size()) {
        return false;
    }
    return layer.loadedSubtrees[subtreeLevel].count(mortonIndex) > 0;
}

TileAvailabilityState QuantizedMeshTerrainProvider::availabilityStateInLayer(
    const LayerConfig& layer,
    const TileKey& key) const {
    if (!isGeographicTmsTileInRange(key)) {
        return TileAvailabilityState::NotAvailable;
    }
    if (!layer.hasAvailability) return TileAvailabilityState::Available;

    if (maximumAvailableLevelAtTileCenter(layer, key) >=
        static_cast<uint32_t>(key.z)) {
        return TileAvailabilityState::Available;
    }

    if (layer.availabilityLevels <= 0) {
        return TileAvailabilityState::NotAvailable;
    }

    if (key.z % layer.availabilityLevels == 0 &&
        isSubtreeLoadedInLayer(
            layer,
            key.z / layer.availabilityLevels,
            mortonEncode2D(static_cast<uint32_t>(key.x),
                           static_cast<uint32_t>(key.y)))) {
        return TileAvailabilityState::NotAvailable;
    }

    const int levelLeft = key.z % layer.availabilityLevels;
    const int subtreeLevel = key.z / layer.availabilityLevels;
    const uint32_t subtreeX = static_cast<uint32_t>(key.x >> levelLeft);
    const uint32_t subtreeY = static_cast<uint32_t>(key.y >> levelLeft);
    if (isSubtreeLoadedInLayer(
            layer, subtreeLevel, mortonEncode2D(subtreeX, subtreeY))) {
        return TileAvailabilityState::NotAvailable;
    }

    return TileAvailabilityState::Unknown;
}

TileAvailabilityState QuantizedMeshTerrainProvider::availabilityState(
    const TileKey& key) const {
    if (layers_.empty()) {
        if (key.z < minZoom_ || key.z > maxZoom_) {
            return TileAvailabilityState::NotAvailable;
        }
        if (!isGeographicTmsTileInRange(key)) {
            return TileAvailabilityState::NotAvailable;
        }
        if (!hasAvailability_) return TileAvailabilityState::Available;

        LayerConfig legacy;
        legacy.urlTemplate = urlTemplate_;
        legacy.version = version_;
        legacy.extensionsToRequest = extensionsToRequest_;
        legacy.availabilityRanges = availabilityRanges_;
        legacy.loadedSubtrees = loadedSubtrees_;
        legacy.hasAvailability = hasAvailability_;
        legacy.availabilityLevels = availabilityLevels_;
        legacy.minZoom = minZoom_;
        legacy.maxZoom = maxZoom_;
        return availabilityStateInLayer(legacy, key);
    }

    bool anyUnknown = false;
    for (const LayerConfig& layer : layers_) {
        const TileAvailabilityState state = availabilityStateInLayer(layer, key);
        if (state == TileAvailabilityState::Available) {
            return TileAvailabilityState::Available;
        }
        anyUnknown |= state == TileAvailabilityState::Unknown;
    }
    return anyUnknown ? TileAvailabilityState::Unknown
                      : TileAvailabilityState::NotAvailable;
}

const QuantizedMeshTerrainProvider::LayerConfig*
QuantizedMeshTerrainProvider::firstAvailableLayer(const TileKey& key) const {
    const size_t index = firstAvailableLayerIndex(key);
    if (index < layers_.size()) {
        return &layers_[index];
    }
    return layers_.empty() ? nullptr : &layers_.front();
}

QuantizedMeshTerrainProvider::LayerConfig*
QuantizedMeshTerrainProvider::firstAvailableLayer(const TileKey& key) {
    const size_t index = firstAvailableLayerIndex(key);
    if (index < layers_.size()) {
        return &layers_[index];
    }
    return layers_.empty() ? nullptr : &layers_.front();
}

size_t QuantizedMeshTerrainProvider::firstAvailableLayerIndex(
    const TileKey& key) const {
    for (size_t i = 0; i < layers_.size(); ++i) {
        if (availabilityStateInLayer(layers_[i], key) ==
            TileAvailabilityState::Available) {
            return i;
        }
    }
    return layers_.size();
}

std::vector<QuantizedMeshTerrainProvider::LayerAvailabilityRequest>
QuantizedMeshTerrainProvider::collectUnderlyingLayerAvailabilityRequests(
    const TileKey& key) const {
    std::vector<LayerAvailabilityRequest> requests;
    const size_t firstIndex = firstAvailableLayerIndex(key);
    if (firstIndex >= layers_.size()) {
        return requests;
    }

    for (size_t i = firstIndex + 1; i < layers_.size(); ++i) {
        const LayerConfig& layer = layers_[i];
        if (layer.availabilityLevels < 1 ||
            key.z % layer.availabilityLevels != 0) {
            continue;
        }

        const int subtreeLevel = key.z / layer.availabilityLevels;
        const uint64_t subtreeMorton =
            mortonEncode2D(static_cast<uint32_t>(key.x),
                           static_cast<uint32_t>(key.y));
        if (isSubtreeLoadedInLayer(layer, subtreeLevel, subtreeMorton)) {
            continue;
        }

        requests.push_back(LayerAvailabilityRequest{
            i, key, buildUrlForLayer(layer, key)});
    }
    return requests;
}

std::string QuantizedMeshTerrainProvider::id() const {
    std::ostringstream oss;
    oss << "qmesh-" << std::hash<std::string>{}(urlTemplate_);
    return oss.str();
}

std::string QuantizedMeshTerrainProvider::buildUrlForLayer(
    const LayerConfig& layer,
    const TileKey& key) const {
    const int tilesAtZoom = 1 << key.z;
    const int urlY = flipYForUrl_
        ? std::clamp(tilesAtZoom - 1 - key.y, 0, tilesAtZoom - 1)
        : key.y;
    const std::string level = std::to_string(key.z);
    std::string url = substituteTemplateParameters(
        layer.urlTemplate,
        [&key, &layer, &level, urlY](const std::string& placeholder) {
            if (placeholder == "level" || placeholder == "z") {
                return level;
            }
            if (placeholder == "x") {
                return std::to_string(key.x);
            }
            if (placeholder == "y") {
                return std::to_string(urlY);
            }
            if (placeholder == "version") {
                return layer.version;
            }
            return placeholder;
        });
    setQueryParameter(url, "extensions", layer.extensionsToRequest);
    return url;
}

std::string QuantizedMeshTerrainProvider::buildUrl(const TileKey& key) const {
    if (!layers_.empty()) {
        const LayerConfig* layer = firstAvailableLayer(key);
        if (layer) {
            return buildUrlForLayer(*layer, key);
        }
    }

    LayerConfig legacy;
    legacy.urlTemplate = urlTemplate_;
    legacy.version = version_;
    legacy.extensionsToRequest = extensionsToRequest_;
    return buildUrlForLayer(legacy, key);
}

void QuantizedMeshTerrainProvider::requestTile(const TileKey& key,
                                                CancellationToken token,
                                                HeightmapCallback callback) {
    std::string url = buildUrl(key);
    std::vector<LayerAvailabilityRequest> availabilityRequests =
        collectUnderlyingLayerAvailabilityRequests(key);
    AsyncSystem::pool().enqueue(
        [this, url, key,
         availabilityRequests = std::move(availabilityRequests),
         token = std::move(token),
         callback = std::move(callback)]() mutable {
            if (token.isCancelled()) {
                callback(key, TerrainTileLoadResult::cancelled());
                return;
            }
            auto body = httpGet(url);
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_INFO, "QMTerrain",
                "requestTile: z=%d x=%d y=%d body=%zu canceled=%d",
                key.z, key.x, key.y, body.size(), token.isCancelled());
#endif
            if (token.isCancelled()) {
                callback(key, TerrainTileLoadResult::cancelled());
                return;
            }
            if (body.empty()) {
                callback(key, TerrainTileLoadResult::retryLater());
                return;
            }
            auto hm = decodeTile(body.data(), body.size());
            if (hm) {
                for (const LayerAvailabilityRequest& request :
                     availabilityRequests) {
                    DecodedHeightmap::QuantizedMeshAvailabilityUpdate update;
                    update.layerIndex = static_cast<int>(request.layerIndex);
                    update.subtreeKey = request.subtreeKey;

                    auto metadataBody = httpGet(request.url);
                    if (!metadataBody.empty()) {
                        update.metadataAvailability =
                            QuantizedMeshParser::parseMetadataAvailability(
                                metadataBody.data(),
                                metadataBody.size());
                    }

                    // cesium-native addRectangleAvailabilityToLayer marks the
                    // subtree loaded even when metadata loading/parsing yields
                    // no rectangles.
                    hm->quantizedMeshAvailabilityUpdates.push_back(
                        std::move(update));
                }
            }
            callback(key, TerrainTileLoadResult::success(std::move(hm)));
        });
}

void QuantizedMeshTerrainProvider::addAvailabilityRects(
    int level, const std::vector<std::array<int, 4>>& rects) {
    if (level < 0) return;
    if (!layers_.empty()) {
        addAvailabilityRectsToLayer(layers_.front(), level, rects);
        syncLegacyFieldsFromPrimaryLayer();
        return;
    }
    hasAvailability_ = true;
    if (static_cast<size_t>(level) >= availabilityRanges_.size()) {
        availabilityRanges_.resize(static_cast<size_t>(level) + 1);
    }
    auto& ranges = availabilityRanges_[static_cast<size_t>(level)];
    for (const auto& r : rects) {
        ranges.push_back(r);
    }
}

void QuantizedMeshTerrainProvider::addAvailabilityRectsToLayer(
    LayerConfig& layer,
    int level,
    const std::vector<std::array<int, 4>>& rects) {
    if (level < 0) return;
    layer.hasAvailability = true;
    if (static_cast<size_t>(level) >= layer.availabilityRanges.size()) {
        layer.availabilityRanges.resize(static_cast<size_t>(level) + 1);
    }
    auto& ranges = layer.availabilityRanges[static_cast<size_t>(level)];
    for (const auto& r : rects) {
        ranges.push_back(r);
    }
}

void QuantizedMeshTerrainProvider::addAvailabilityRectsForTile(
    const TileKey& subtreeKey,
    int level,
    const std::vector<std::array<int, 4>>& rects) {
    if (layers_.empty()) {
        addAvailabilityRects(level, rects);
        return;
    }

    LayerConfig* layer = firstAvailableLayer(subtreeKey);
    if (!layer || level < 0) return;
    addAvailabilityRectsToLayer(*layer, level, rects);
    syncLegacyFieldsFromPrimaryLayer();
}

bool QuantizedMeshTerrainProvider::isSubtreeLoaded(
    int subtreeLevel, uint64_t mortonIndex) const {
    if (!layers_.empty()) {
        return isSubtreeLoadedInLayer(
            layers_.front(), subtreeLevel, mortonIndex);
    }
    if (subtreeLevel < 0) {
        return false;
    }
    if (availabilityLevels_ > 0 &&
        static_cast<size_t>(subtreeLevel) >= loadedSubtrees_.size()) {
        return true;
    }
    if (static_cast<size_t>(subtreeLevel) >= loadedSubtrees_.size()) return false;
    return loadedSubtrees_[subtreeLevel].count(mortonIndex) > 0;
}

void QuantizedMeshTerrainProvider::markSubtreeLoadedInLayer(
    LayerConfig& layer,
    int subtreeLevel,
    uint64_t mortonIndex) {
    if (subtreeLevel < 0) return;
    layer.hasAvailability = true;
    if (static_cast<size_t>(subtreeLevel) >= layer.loadedSubtrees.size()) {
        if (layer.availabilityLevels > 0) {
            return;
        }
        layer.loadedSubtrees.resize(static_cast<size_t>(subtreeLevel) + 1);
    }
    layer.loadedSubtrees[static_cast<size_t>(subtreeLevel)].insert(mortonIndex);
}

void QuantizedMeshTerrainProvider::markSubtreeLoaded(
    int subtreeLevel, uint64_t mortonIndex) {
    if (subtreeLevel < 0) return;
    if (!layers_.empty()) {
        markSubtreeLoadedInLayer(layers_.front(), subtreeLevel, mortonIndex);
        syncLegacyFieldsFromPrimaryLayer();
        return;
    }
    hasAvailability_ = true;
    if (static_cast<size_t>(subtreeLevel) >= loadedSubtrees_.size()) {
        if (availabilityLevels_ > 0) {
            return;
        }
        loadedSubtrees_.resize(subtreeLevel + 1);
    }
    loadedSubtrees_[subtreeLevel].insert(mortonIndex);
}

void QuantizedMeshTerrainProvider::markSubtreeLoadedForTile(
    const TileKey& subtreeKey) {
    if (layers_.empty()) {
        if (availabilityLevels_ > 0) {
            markSubtreeLoaded(
                subtreeKey.z / availabilityLevels_,
                mortonEncode2D(static_cast<uint32_t>(subtreeKey.x),
                               static_cast<uint32_t>(subtreeKey.y)));
        }
        return;
    }

    LayerConfig* layer = firstAvailableLayer(subtreeKey);
    if (!layer || layer->availabilityLevels <= 0) return;
    markSubtreeLoadedInLayer(
        *layer,
        subtreeKey.z / layer->availabilityLevels,
        mortonEncode2D(static_cast<uint32_t>(subtreeKey.x),
                       static_cast<uint32_t>(subtreeKey.y)));
    syncLegacyFieldsFromPrimaryLayer();
}

void QuantizedMeshTerrainProvider::applyAvailabilityUpdates(
    const DecodedHeightmap& heightmap) {
    if (heightmap.quantizedMeshAvailabilityUpdates.empty()) {
        return;
    }

    for (const auto& update : heightmap.quantizedMeshAvailabilityUpdates) {
        if (update.layerIndex < 0 ||
            static_cast<size_t>(update.layerIndex) >= layers_.size()) {
            continue;
        }

        LayerConfig& layer = layers_[static_cast<size_t>(update.layerIndex)];
        for (const auto& r : update.metadataAvailability) {
            const int absLevel = update.subtreeKey.z + 1 + r[0];
            if (absLevel >= 0) {
                addAvailabilityRectsToLayer(
                    layer, absLevel, {{r[1], r[2], r[3], r[4]}});
            }
        }

        if (layer.availabilityLevels > 0) {
            markSubtreeLoadedInLayer(
                layer,
                update.subtreeKey.z / layer.availabilityLevels,
                mortonEncode2D(static_cast<uint32_t>(update.subtreeKey.x),
                               static_cast<uint32_t>(update.subtreeKey.y)));
        }
    }
    syncLegacyFieldsFromPrimaryLayer();
}

bool QuantizedMeshTerrainProvider::isAvailabilityBoundaryLevel(int level) const {
    if (layers_.empty()) {
        return availabilityLevels_ > 0 && level % availabilityLevels_ == 0;
    }
    for (const LayerConfig& layer : layers_) {
        if (layer.availabilityLevels > 0 &&
            level % layer.availabilityLevels == 0) {
            return true;
        }
    }
    return false;
}

std::unique_ptr<DecodedHeightmap> QuantizedMeshTerrainProvider::decodeTile(
    const uint8_t* data, size_t len) {
    // Rasterize to regular heightmap grid for sampleHeight queries
    auto hm = QuantizedMeshParser::parseAndRasterize(data, len, tileSize_ - 1);
    if (!hm) return nullptr;

    // Preserve raw binary for on-demand triangulated mesh reconstruction
    hm->rawData.assign(data, data + len);
    return hm;
}

std::vector<uint8_t> QuantizedMeshTerrainProvider::httpGet(const std::string& url) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "QMTerrain", "httpGet ENTER: %s", url.c_str());
#endif
    // Check shared LRU cache
    auto cached = HttpCache::shared().get(url);
#ifdef __ANDROID__
    if (!cached.empty())
        __android_log_print(ANDROID_LOG_INFO, "QMTerrain",
            "httpGet cache hit: %zu bytes", cached.size());
#endif
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
        std::mutex mtx;
        std::condition_variable cv;
        bool done = false;
        platformBridge_->get(url, [&](int code, std::vector<uint8_t> body) {
            if (code == 200) result = std::move(body);
            { std::lock_guard<std::mutex> lk(mtx); done = true; }
            cv.notify_one();
        });
        { std::unique_lock<std::mutex> lk(mtx);
          cv.wait_for(lk, std::chrono::seconds(20), [&]{ return done; }); }
        if (!result.empty()) HttpCache::shared().put(url, result);
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "QMTerrain",
            "httpGet bridge: %zu bytes", result.size());
#endif
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
    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    std::vector<uint8_t> result =
        (res == CURLE_OK && httpCode == 200) ? std::move(body) : std::vector<uint8_t>{};
    if (!result.empty()) HttpCache::shared().put(url, result);
    return result;
#else
    (void)url;
    return {};
#endif
}

} // namespace earth_engine
