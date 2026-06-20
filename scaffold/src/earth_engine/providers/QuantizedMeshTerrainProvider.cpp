#include "QuantizedMeshTerrainProvider.h"
#include "QuantizedMeshLayerJsonFetcher.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include "../core/async/AsyncSystem.h"
#include "../core/cache/HttpCache.h"
#include "../platform/bridge/CurlMultiRequestScheduler.h"
#include "../platform/bridge/PlatformBridge.h"
#include "../terrain/QuantizedMeshParser.h"
#include <nlohmann/json.hpp>

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

namespace {

struct RequestCompletionGuard {
    std::atomic<int>& completed;
    ~RequestCompletionGuard() {
        completed.fetch_add(1, std::memory_order_relaxed);
    }
};

constexpr const char* kFileUrlPrefix = "file://";

bool isFileUrl(const std::string& url) {
    return url.rfind(kFileUrlPrefix, 0) == 0;
}

std::vector<uint8_t> readFileUrl(const std::string& url) {
    if (!isFileUrl(url)) {
        return {};
    }
    auto cached = HttpCache::shared().get(url);
    if (!cached.empty()) {
        return cached;
    }
    std::ifstream in(url.substr(std::strlen(kFileUrlPrefix)), std::ios::binary);
    if (!in) {
        return {};
    }
    std::vector<uint8_t> data{
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>()};
    HttpCache::shared().put(url, data);
    return data;
}

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

Rectangle geographicTmsRectangle(const TileKey& key) {
    constexpr double kPi = 3.14159265358979323846264338327950288;
    const double xTilesAtZ = std::ldexp(1.0, key.z + 1);
    const double yTilesAtZ = std::ldexp(1.0, key.z);
    const double west =
        static_cast<double>(key.x) / xTilesAtZ * 2.0 * kPi - kPi;
    const double east =
        static_cast<double>(key.x + 1) / xTilesAtZ * 2.0 * kPi - kPi;
    const double south =
        -0.5 * kPi + static_cast<double>(key.y) / yTilesAtZ * kPi;
    const double north =
        -0.5 * kPi + static_cast<double>(key.y + 1) / yTilesAtZ * kPi;
    return Rectangle(west, south, east, north);
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

std::string schemeIdForLayerProjection(const std::string& projection) {
    if (projection == "EPSG:4326") return "Geographic-TMS";
    if (projection == "EPSG:3857") return "XYZ-WebMercator";
    return {};
}

bool isTileInLayerRange(const TileKey& key, const std::string& schemeId) {
    if (key.schemeId != schemeId || key.z < 0 ||
        key.x < 0 || key.y < 0) {
        return false;
    }

    const bool geographic = schemeId == "Geographic-TMS";
    const double xTiles = geographic
        ? std::ldexp(1.0, key.z + 1)
        : std::ldexp(1.0, key.z);
    const double yTiles = std::ldexp(1.0, key.z);
    return static_cast<double>(key.x) < xTiles &&
           static_cast<double>(key.y) < yTiles;
}

bool isCesiumSuccessfulHttpStatus(int statusCode) {
    return statusCode == 0 || (statusCode >= 200 && statusCode < 300);
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

int jsonInt32OrDefault(const nlohmann::json& object,
                       const char* name,
                       int defaultValue) {
    auto it = object.find(name);
    if (it == object.end() || !it->is_number_integer()) {
        return defaultValue;
    }

    const int64_t value = it->get<int64_t>();
    if (value < static_cast<int64_t>(std::numeric_limits<int>::min()) ||
        value > static_cast<int64_t>(std::numeric_limits<int>::max())) {
        return defaultValue;
    }
    return static_cast<int>(value);
}

std::optional<int> jsonInt32(const nlohmann::json& object, const char* name) {
    auto it = object.find(name);
    if (it == object.end() || !it->is_number_integer()) {
        return std::nullopt;
    }

    const int64_t value = it->get<int64_t>();
    if (value < static_cast<int64_t>(std::numeric_limits<int>::min()) ||
        value > static_cast<int64_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

std::string jsonStringOrEmpty(const nlohmann::json& object, const char* name) {
    auto it = object.find(name);
    if (it == object.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

std::string jsonStringOrDefault(
    const nlohmann::json& object,
    const char* name,
    const std::string& defaultValue) {
    auto it = object.find(name);
    if (it == object.end() || !it->is_string()) {
        return defaultValue;
    }
    return it->get<std::string>();
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

    size_t pos = queryStart + 1;
    bool found = false;
    while (pos <= url.size()) {
        const size_t valueStart = url.find('=', pos);
        const size_t paramEnd = url.find('&', pos);
        const size_t keyEnd =
            valueStart == std::string::npos ||
                    (paramEnd != std::string::npos && valueStart > paramEnd)
                ? paramEnd
                : valueStart;
        if (keyEnd == std::string::npos) break;
        if (url.compare(pos, keyEnd - pos, name) == 0) {
            found = true;
            break;
        }
        if (paramEnd == std::string::npos) {
            break;
        }
        pos = paramEnd + 1;
    }
    if (!found) {
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
    QuantizedMeshLayerJsonFetcher fetcher(platformBridge_);
    auto bytes = fetcher.fetchBlocking(layerJsonUrl);
    if (bytes.empty()) return false;
    std::string body(bytes.begin(), bytes.end());
    return configureFromLayerJson(body, layerJsonUrl);
}

bool QuantizedMeshTerrainProvider::configureFromLayerJson(
    const std::string& layerJson,
    const std::string& layerJsonUrl) {
    const auto oldLayers = layers_;
    const auto oldUrlTemplate = urlTemplate_;
    const auto oldAttribution = attribution_;
    const auto oldLayerJsonUrl = layerJsonUrl_;
    const auto oldSchemeId = schemeId_;
    const auto oldVersion = version_;
    const auto oldExtensionsToRequest = extensionsToRequest_;
    const auto oldAvailabilityRanges = availabilityRanges_;
    const auto oldLoadedSubtrees = loadedSubtrees_;
    const bool oldHasAvailability = hasAvailability_;
    const int oldAvailabilityLevels = availabilityLevels_;
    const int oldMinZoom = minZoom_;
    const int oldMaxZoom = maxZoom_;
    const auto restorePreviousState = [&]() {
        layers_ = oldLayers;
        urlTemplate_ = oldUrlTemplate;
        attribution_ = oldAttribution;
        layerJsonUrl_ = oldLayerJsonUrl;
        schemeId_ = oldSchemeId;
        version_ = oldVersion;
        extensionsToRequest_ = oldExtensionsToRequest;
        availabilityRanges_ = oldAvailabilityRanges;
        loadedSubtrees_ = oldLoadedSubtrees;
        hasAvailability_ = oldHasAvailability;
        availabilityLevels_ = oldAvailabilityLevels;
        minZoom_ = oldMinZoom;
        maxZoom_ = oldMaxZoom;
    };

    try {
        auto j = nlohmann::json::parse(layerJson);
        if (schemeIdForLayerProjection(
                jsonStringOrDefault(j, "projection", "EPSG:4326")).empty()) {
            return false;
        }
        layers_.clear();
        if (!appendLayerFromJson(j, layerJsonUrl)) {
            restorePreviousState();
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
        restorePreviousState();
        return false;
    }
}

bool QuantizedMeshTerrainProvider::appendLayerFromJson(
    const nlohmann::json& j,
    const std::string& layerJsonUrl,
    const std::string& forcedSchemeId) {
    const std::vector<std::string> tileTemplates = jsonStringArray(j, "tiles");
    if (tileTemplates.empty()) {
        return false;
    }

    LayerConfig layer;
    layer.urlTemplate =
        resolveTerrainTemplate(layerJsonUrl, tileTemplates.front());
    layer.layerJsonUrl = layerJsonUrl;
    layer.schemeId = forcedSchemeId.empty()
        ? schemeIdForLayerProjection(
              jsonStringOrDefault(j, "projection", "EPSG:4326"))
        : forcedSchemeId;
    if (layer.schemeId.empty()) {
        return false;
    }
    // cesium-native LayerJsonTerrainLoader reads layer.json maxzoom for
    // availability storage/subtree bookkeeping, but minzoom/maxzoom do not
    // directly gate tile availability. Availability rectangles and loaded
    // metadata subtrees determine whether tiles are requestable.
    layer.minZoom = 0;
    layer.maxZoom = jsonInt32OrDefault(j, "maxzoom", 30);
    layer.version = jsonStringOrEmpty(j, "version");
    layer.attribution = jsonStringOrEmpty(j, "attribution");
    std::vector<std::string> knownExtensions{
        "octvertexnormals",
        "metadata"
    };
    if (waterMaskEnabled_) {
        knownExtensions.push_back("watermask");
    }
    layer.extensionsToRequest = createExtensionsQueryParameter(
        knownExtensions,
        jsonStringArray(j, "extensions"));

    const std::optional<int> metadataAvailability =
        jsonInt32(j, "metadataAvailability");
    layer.availabilityLevels = metadataAvailability.value_or(-1);
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
    if (!metadataAvailability &&
        j.contains("available") && j["available"].is_array()) {
        int availabilityLevel = 0;
        for (const auto& levelRanges : j["available"]) {
            if (!levelRanges.is_array()) continue;
            if (static_cast<size_t>(availabilityLevel) >=
                layer.availabilityRanges.size()) {
                layer.availabilityRanges.resize(
                    static_cast<size_t>(availabilityLevel) + 1);
            }
            for (const auto& range : levelRanges) {
                if (!range.is_object()) continue;
                layer.availabilityRanges[static_cast<size_t>(availabilityLevel)]
                    .push_back({
                    jsonUint32OrDefault(range, "startX"),
                    jsonUint32OrDefault(range, "startY"),
                    jsonUint32OrDefault(range, "endX"),
                    jsonUint32OrDefault(range, "endY")
                });
            }
            ++availabilityLevel;
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
    std::string parentUrl = jsonStringOrDefault(j, "parentUrl", "");
    if (parentUrl.empty()) {
        return true;
    }

    std::string resolvedUrl = resolveParentLayerJsonUrl(layerJsonUrl, parentUrl);
    QuantizedMeshLayerJsonFetcher fetcher(platformBridge_);
    auto bytes = fetcher.fetchBlocking(resolvedUrl);
    if (bytes.empty()) {
        return false;
    }

    try {
        const std::string body(bytes.begin(), bytes.end());
        auto parent = nlohmann::json::parse(body);
        const std::string parentSchemeId =
            layers_.empty() ? std::string{} : layers_.front().schemeId;
        if (!appendLayerFromJson(parent, resolvedUrl, parentSchemeId)) {
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
    schemeId_ = primary.schemeId;
    version_ = primary.version;
    extensionsToRequest_ = primary.extensionsToRequest;
    availabilityRanges_ = primary.availabilityRanges;
    loadedSubtrees_ = primary.loadedSubtrees;
    hasAvailability_ = primary.hasAvailability;
    availabilityLevels_ = primary.availabilityLevels;
    minZoom_ = primary.minZoom;
    maxZoom_ = primary.maxZoom;
    attribution_.clear();
    std::unordered_set<std::string> seenAttributions;
    for (const LayerConfig& layer : layers_) {
        minZoom_ = std::min(minZoom_, layer.minZoom);
        maxZoom_ = std::max(maxZoom_, layer.maxZoom);
        if (layer.attribution.empty()) continue;
        if (!seenAttributions.insert(layer.attribution).second) continue;
        if (!attribution_.empty()) {
            attribution_ += "\n";
        }
        attribution_ += layer.attribution;
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

    const bool geographic = layer.schemeId == "Geographic-TMS";
    const double keyXTiles = geographic
        ? std::ldexp(1.0, key.z + 1)
        : std::ldexp(1.0, key.z);
    const double keyYTiles = std::ldexp(1.0, key.z);
    const double centerX = (static_cast<double>(key.x) + 0.5) / keyXTiles;
    const double centerY = (static_cast<double>(key.y) + 0.5) / keyYTiles;

    for (size_t levelIndex = 0; levelIndex < layer.availabilityRanges.size(); ++levelIndex) {
        const auto& ranges = layer.availabilityRanges[levelIndex];
        if (ranges.empty()) continue;

        const int level = static_cast<int>(levelIndex);
        const double xTiles = geographic
            ? std::ldexp(1.0, level + 1)
            : std::ldexp(1.0, level);
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
    if (!isTileInLayerRange(key, layer.schemeId)) {
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
        if (!isTileInLayerRange(key, schemeId_)) {
            return TileAvailabilityState::NotAvailable;
        }
        if (!hasAvailability_) return TileAvailabilityState::Available;

        LayerConfig legacy;
        legacy.urlTemplate = urlTemplate_;
        legacy.schemeId = schemeId_;
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

int QuantizedMeshTerrainProvider::estimatedRequestFanout(
    const TileKey& key) const {
    return 1 + static_cast<int>(
        collectUnderlyingLayerAvailabilityRequests(key).size());
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
    legacy.schemeId = schemeId_;
    legacy.version = version_;
    legacy.extensionsToRequest = extensionsToRequest_;
    return buildUrlForLayer(legacy, key);
}

void QuantizedMeshTerrainProvider::requestTile(const TileKey& key,
                                                CancellationToken token,
                                                HeightmapCallback callback,
                                                HttpRequestPriority priority) {
    std::string url = buildUrl(key);
    std::vector<LayerAvailabilityRequest> availabilityRequests =
        collectUnderlyingLayerAvailabilityRequests(key);
    if (platformBridge_) {
        requestsStarted_.fetch_add(1, std::memory_order_relaxed);
        if (token.isCancelled()) {
            requestsCompleted_.fetch_add(1, std::memory_order_relaxed);
            callback(key, TerrainTileLoadResult::cancelled());
            return;
        }

        auto requestHandle =
            std::make_shared<std::unique_ptr<HttpRequest>>();
        *requestHandle = platformBridge_->get(
            url,
            [this,
             key,
             availabilityRequests = std::move(availabilityRequests),
             token = std::move(token),
             priority,
             callback = std::move(callback),
             requestHandle](int statusCode, std::vector<uint8_t> body) mutable {
                (void)requestHandle;
                handleAsyncTileBody(
                    key,
                    std::move(availabilityRequests),
                    std::move(token),
                    std::move(callback),
                    priority,
                    statusCode,
                    std::move(body),
                    true);
            },
            {priority});
        return;
    }

    requestsStarted_.fetch_add(1, std::memory_order_relaxed);
    if (token.isCancelled()) {
        requestsCompleted_.fetch_add(1, std::memory_order_relaxed);
        callback(key, TerrainTileLoadResult::cancelled());
        return;
    }

    if (isFileUrl(url)) {
        AsyncSystem::pool().enqueue(
            [this,
             url,
             key,
             availabilityRequests = std::move(availabilityRequests),
             token = std::move(token),
             priority,
             callback = std::move(callback)]() mutable {
                if (token.isCancelled()) {
                    callback(key, TerrainTileLoadResult::cancelled());
                    return;
                }

                std::vector<uint8_t> body = readFileUrl(url);
	#ifdef __ANDROID__
	                __android_log_print(ANDROID_LOG_INFO, "QMTerrain",
	                    "requestTile: z=%d x=%d y=%d body=%zu canceled=%d",
	                    key.z, key.x, key.y, body.size(), token.isCancelled());
	#endif
                handleAsyncTileBody(
                    key,
                    std::move(availabilityRequests),
                    std::move(token),
                    std::move(callback),
                    priority,
                    body.empty() ? 0 : 200,
                    std::move(body),
                    false);
            });
        return;
    }

    auto requestHandle =
        std::make_shared<std::unique_ptr<HttpRequest>>();
    *requestHandle = CurlMultiRequestScheduler::shared().get(
        url,
        [this,
         key,
         availabilityRequests = std::move(availabilityRequests),
         token = std::move(token),
         priority,
         callback = std::move(callback),
         requestHandle](int statusCode, std::vector<uint8_t> body) mutable {
            (void)requestHandle;
            handleAsyncTileBody(
                key,
                std::move(availabilityRequests),
                std::move(token),
                std::move(callback),
                priority,
                statusCode,
                std::move(body),
                false);
        },
        {priority});
}

void QuantizedMeshTerrainProvider::handleAsyncTileBody(
    const TileKey& key,
    std::vector<LayerAvailabilityRequest> availabilityRequests,
    CancellationToken token,
    HeightmapCallback callback,
    HttpRequestPriority priority,
    int statusCode,
    std::vector<uint8_t> body,
    bool usePlatformBridge) {
    auto availabilityRequestsPtr =
        std::make_shared<std::vector<LayerAvailabilityRequest>>(
            std::move(availabilityRequests));
    auto tokenPtr = std::make_shared<CancellationToken>(std::move(token));
    auto callbackPtr =
        std::make_shared<HeightmapCallback>(std::move(callback));
    auto bodyPtr =
        std::make_shared<std::vector<uint8_t>>(std::move(body));

    if (!isCesiumSuccessfulHttpStatus(statusCode) ||
        bodyPtr->empty() ||
        tokenPtr->isCancelled() ||
        availabilityRequestsPtr->empty()) {
        finalizeAsyncTileRequest(
            key,
            availabilityRequestsPtr,
            tokenPtr,
            callbackPtr,
            bodyPtr,
            statusCode,
            {});
        return;
    }

    requestAsyncMetadataAndFinalize(
        key,
        availabilityRequestsPtr,
        tokenPtr,
        callbackPtr,
        bodyPtr,
        statusCode,
        priority,
        usePlatformBridge);
}

void QuantizedMeshTerrainProvider::requestAsyncMetadataAndFinalize(
    TileKey key,
    std::shared_ptr<std::vector<LayerAvailabilityRequest>>
        availabilityRequests,
    std::shared_ptr<CancellationToken> token,
    std::shared_ptr<HeightmapCallback> callback,
    std::shared_ptr<std::vector<uint8_t>> body,
    int statusCode,
    HttpRequestPriority priority,
    bool usePlatformBridge) {
    struct MetadataFetchState {
        std::mutex mutex;
        std::vector<std::vector<uint8_t>> bodies;
        std::vector<std::unique_ptr<HttpRequest>> handles;
        size_t remaining = 0;
        bool finalized = false;
    };
    auto metadataState = std::make_shared<MetadataFetchState>();
    metadataState->bodies.resize(availabilityRequests->size());
    metadataState->handles.resize(availabilityRequests->size());
    metadataState->remaining = availabilityRequests->size();

    for (size_t i = 0; i < availabilityRequests->size(); ++i) {
        auto metadataCallback =
            [this,
             metadataState,
             key,
             availabilityRequests,
             token,
             callback,
             body,
             statusCode,
             i](int metadataStatusCode,
                std::vector<uint8_t> metadataBody) mutable {
                std::vector<std::vector<uint8_t>> metadataBodies;
                bool shouldFinalize = false;
                {
                    std::lock_guard<std::mutex> lock(metadataState->mutex);
                    if (!metadataState->finalized &&
                        !token->isCancelled() &&
                        isCesiumSuccessfulHttpStatus(metadataStatusCode) &&
                        !metadataBody.empty()) {
                        metadataState->bodies[i] = std::move(metadataBody);
                    }
                    if (metadataState->remaining > 0) {
                        --metadataState->remaining;
                    }
                    if (metadataState->remaining == 0 &&
                        !metadataState->finalized) {
                        metadataState->finalized = true;
                        metadataBodies = std::move(metadataState->bodies);
                        shouldFinalize = true;
                    }
                }
                if (shouldFinalize) {
                    finalizeAsyncTileRequest(
                        key,
                        availabilityRequests,
                        token,
                        callback,
                        body,
                        statusCode,
                        std::move(metadataBodies));
                }
            };

        const std::string& metadataUrl = (*availabilityRequests)[i].url;
        if (!usePlatformBridge && isFileUrl(metadataUrl)) {
            AsyncSystem::pool().enqueue(
                [metadataUrl,
                 metadataCallback = std::move(metadataCallback)]() mutable {
                    std::vector<uint8_t> metadataBody =
                        readFileUrl(metadataUrl);
                    metadataCallback(
                        metadataBody.empty() ? 0 : 200,
                        std::move(metadataBody));
                });
        } else if (usePlatformBridge) {
            metadataState->handles[i] = platformBridge_->get(
                metadataUrl,
                std::move(metadataCallback),
                {priority});
        } else {
            metadataState->handles[i] =
                CurlMultiRequestScheduler::shared().get(
                    metadataUrl,
                    std::move(metadataCallback),
                    {priority});
        }
    }
}

void QuantizedMeshTerrainProvider::finalizeAsyncTileRequest(
    TileKey key,
    std::shared_ptr<std::vector<LayerAvailabilityRequest>>
        availabilityRequests,
    std::shared_ptr<CancellationToken> token,
    std::shared_ptr<HeightmapCallback> callback,
    std::shared_ptr<std::vector<uint8_t>> body,
    int statusCode,
    std::vector<std::vector<uint8_t>> metadataBodies) {
    AsyncSystem::pool().enqueue(
        [this,
         key,
         availabilityRequests,
         token,
         callback,
         body,
         statusCode,
         metadataBodies = std::move(metadataBodies)]() mutable {
            RequestCompletionGuard completion{requestsCompleted_};
            if (token->isCancelled()) {
                (*callback)(key, TerrainTileLoadResult::cancelled());
                return;
            }
            if (!isCesiumSuccessfulHttpStatus(statusCode) || body->empty()) {
                (*callback)(key, TerrainTileLoadResult::retryLater());
                return;
            }
#ifdef __ANDROID__
            __android_log_print(
                ANDROID_LOG_INFO,
                "QMTerrain",
                "requestTile: z=%d x=%d y=%d body=%zu canceled=%d",
                key.z,
                key.x,
                key.y,
                body->size(),
                token->isCancelled());
#endif
            auto hm = decodeTile(body->data(), body->size());
            if (hm) {
                hm->surfaceMesh = QuantizedMeshParser::parseToSurfaceTileMesh(
                    body->data(),
                    body->size(),
                    geographicTmsRectangle(key),
                    waterMaskEnabled_);
                for (size_t i = 0; i < availabilityRequests->size(); ++i) {
                    const LayerAvailabilityRequest& request =
                        (*availabilityRequests)[i];
                    DecodedHeightmap::QuantizedMeshAvailabilityUpdate update;
                    update.layerIndex = static_cast<int>(request.layerIndex);
                    update.subtreeKey = request.subtreeKey;

                    if (i < metadataBodies.size() &&
                        !metadataBodies[i].empty()) {
                        update.metadataAvailability =
                            QuantizedMeshParser::parseMetadataAvailability(
                                metadataBodies[i].data(),
                                metadataBodies[i].size());
                    }

                    hm->quantizedMeshAvailabilityUpdates.push_back(
                        std::move(update));
                }
            }
            (*callback)(key, TerrainTileLoadResult::success(std::move(hm)));
        });
}

ProviderRequestDiagnostics
QuantizedMeshTerrainProvider::requestDiagnostics() const {
    ProviderRequestDiagnostics diag;
    diag.requestsStarted = requestsStarted_.load(std::memory_order_relaxed);
    diag.requestsCompleted =
        requestsCompleted_.load(std::memory_order_relaxed);
    diag.activeWorkerBlockingRequests =
        activeWorkerBlockingRequests_.load(std::memory_order_relaxed);
    diag.peakWorkerBlockingRequests =
        peakWorkerBlockingRequests_.load(std::memory_order_relaxed);
    diag.maximumTransportActiveRequests = platformBridge_
        ? platformBridge_->maximumActiveRequests()
        : CurlMultiRequestScheduler::shared().maximumActiveRequests();
    return diag;
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

std::vector<uint8_t> QuantizedMeshTerrainProvider::httpGet(
    const std::string& url,
    HttpRequestPriority priority,
    std::function<bool()> shouldCancel) {
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

    if (isFileUrl(url)) {
        return readFileUrl(url);
    }

    if (platformBridge_) {
        struct WaitState {
            std::vector<uint8_t> result;
            std::mutex mutex;
            std::condition_variable cv;
            bool done = false;
        };
        auto state = std::make_shared<WaitState>();
        auto request = platformBridge_->get(url, [state](int code, std::vector<uint8_t> body) {
            {
                std::lock_guard<std::mutex> lk(state->mutex);
                if (code == 200) state->result = std::move(body);
                state->done = true;
            }
            state->cv.notify_one();
        }, {priority});
        bool done = false;
        {
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(20);
            std::unique_lock<std::mutex> lk(state->mutex);
            while (!state->done && !(shouldCancel && shouldCancel())) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) break;
                state->cv.wait_until(
                    lk,
                    std::min(deadline, now + std::chrono::milliseconds(20)));
            }
            done = state->done;
        }
        if ((!done || (shouldCancel && shouldCancel())) && request) {
            request->cancel();
        }
        if (done && !state->result.empty()) HttpCache::shared().put(url, state->result);
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "QMTerrain",
            "httpGet bridge: %zu bytes", state->result.size());
#endif
        return done ? std::move(state->result) : std::vector<uint8_t>{};
    }

    std::vector<uint8_t> result =
        CurlMultiRequestScheduler::shared().getBlocking(
            url,
            {priority},
            std::chrono::seconds(20),
            std::move(shouldCancel));
    if (!result.empty()) HttpCache::shared().put(url, result);
    return result;
}

} // namespace earth_engine
