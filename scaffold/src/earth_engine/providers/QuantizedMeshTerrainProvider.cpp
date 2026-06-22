#include "QuantizedMeshTerrainProvider.h"
#include "QuantizedMeshLayerJsonFetcher.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include "../core/async/AsyncSystem.h"
#include "../core/cache/HttpCache.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Projection.h"
#include "../core/geodesy/QuadtreeGeometricError.h"
#include "../core/geodesy/WebMercatorProjection.h"
#include "../content/QuantizedMeshContentLoader.h"
#include "../tiling/TileSelectionRootPolicy.h"
#include "../platform/bridge/CurlMultiRequestScheduler.h"
#include "../platform/bridge/PlatformBridge.h"
#include <nlohmann/json.hpp>

#include <sstream>
#include <algorithm>
#include <mutex>
#include <optional>
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
    bool completedOnce = false;
    ~RequestCompletionGuard() {
        complete();
    }
    void complete() {
        if (completedOnce) {
            return;
        }
        completed.fetch_add(1, std::memory_order_relaxed);
        completedOnce = true;
    }
};

constexpr const char* kFileUrlPrefix = "file://";
constexpr double kLooseTerrainMinimumHeight = -1000.0;
constexpr double kLooseTerrainMaximumHeight = 9000.0;

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
      attribution_(std::move(attribution)) {
    resetFallbackLayerFromFields();
}

QuantizedMeshTerrainProvider::~QuantizedMeshTerrainProvider() = default;

QuantizedMeshTerrainProvider::TileRectangleAvailability::Node::Node(
    const TileKey& tileKey,
    const Rectangle& tileExtent,
    Node* parentNode)
    : key(tileKey), extent(tileExtent), parent(parentNode) {}

QuantizedMeshTerrainProvider::TileRectangleAvailability::Node::Node(
    const Node& other)
    : key(other.key),
      extent(other.extent),
      parent(nullptr),
      rectangles(other.rectangles) {
    if (other.ll) ll = QuantizedMeshTerrainProvider::
        TileRectangleAvailability::cloneNode(*other.ll, this);
    if (other.lr) lr = QuantizedMeshTerrainProvider::
        TileRectangleAvailability::cloneNode(*other.lr, this);
    if (other.ul) ul = QuantizedMeshTerrainProvider::
        TileRectangleAvailability::cloneNode(*other.ul, this);
    if (other.ur) ur = QuantizedMeshTerrainProvider::
        TileRectangleAvailability::cloneNode(*other.ur, this);
}

QuantizedMeshTerrainProvider::TileRectangleAvailability::Node&
QuantizedMeshTerrainProvider::TileRectangleAvailability::Node::operator=(
    const Node& other) {
    if (this == &other) return *this;
    key = other.key;
    extent = other.extent;
    rectangles = other.rectangles;
    ll = other.ll ? QuantizedMeshTerrainProvider::
        TileRectangleAvailability::cloneNode(*other.ll, this) : nullptr;
    lr = other.lr ? QuantizedMeshTerrainProvider::
        TileRectangleAvailability::cloneNode(*other.lr, this) : nullptr;
    ul = other.ul ? QuantizedMeshTerrainProvider::
        TileRectangleAvailability::cloneNode(*other.ul, this) : nullptr;
    ur = other.ur ? QuantizedMeshTerrainProvider::
        TileRectangleAvailability::cloneNode(*other.ur, this) : nullptr;
    QuantizedMeshTerrainProvider::TileRectangleAvailability::
        reparentNodeChildren(*this);
    return *this;
}

QuantizedMeshTerrainProvider::TileRectangleAvailability::
    TileRectangleAvailability(std::string scheme, int maxLevel) {
    reset(std::move(scheme), maxLevel);
}

QuantizedMeshTerrainProvider::TileRectangleAvailability::
    TileRectangleAvailability(const TileRectangleAvailability& other)
    : schemeId(other.schemeId),
      maximumLevel(other.maximumLevel) {
    rootNodes.reserve(other.rootNodes.size());
    for (const auto& node : other.rootNodes) {
        rootNodes.push_back(node ? TileRectangleAvailability::cloneNode(*node, nullptr)
                                 : nullptr);
    }
}

QuantizedMeshTerrainProvider::TileRectangleAvailability&
QuantizedMeshTerrainProvider::TileRectangleAvailability::operator=(
    const TileRectangleAvailability& other) {
    if (this == &other) return *this;
    schemeId = other.schemeId;
    maximumLevel = other.maximumLevel;
    rootNodes.clear();
    rootNodes.reserve(other.rootNodes.size());
    for (const auto& node : other.rootNodes) {
        rootNodes.push_back(node ? TileRectangleAvailability::cloneNode(*node, nullptr)
                                 : nullptr);
    }
    return *this;
}

void QuantizedMeshTerrainProvider::TileRectangleAvailability::reset(
    std::string scheme,
    int maxLevel) {
    schemeId = std::move(scheme);
    maximumLevel = std::max(0, maxLevel);
    rootNodes.clear();

    const int rootX = TileRectangleAvailability::rootTileCountX(schemeId);
    const int rootY = TileRectangleAvailability::rootTileCountY(schemeId);
    rootNodes.reserve(static_cast<size_t>(rootX * rootY));
    for (int y = 0; y < rootY; ++y) {
        for (int x = 0; x < rootX; ++x) {
            const TileKey rootKey{schemeId, 0, x, y};
            rootNodes.push_back(std::make_unique<Node>(
                rootKey,
                TileRectangleAvailability::availabilityTileRectangle(
                    schemeId,
                    0,
                    static_cast<uint32_t>(x),
                    static_cast<uint32_t>(y)),
                nullptr));
        }
    }
}

void QuantizedMeshTerrainProvider::TileRectangleAvailability::
    addAvailableTileRange(int level, const TileAvailabilityRect& range) {
    if (level < 0 || rootNodes.empty()) return;
    const Rectangle lowerLeft = TileRectangleAvailability::availabilityTileRectangle(
        schemeId,
        level,
        range[0],
        range[1]);
    const Rectangle upperRight = TileRectangleAvailability::availabilityTileRectangle(
        schemeId,
        level,
        range[2],
        range[3]);
    const RectangleWithLevel rectangle{
        static_cast<uint32_t>(level),
        Rectangle(
            lowerLeft.west(),
            lowerLeft.south(),
            upperRight.east(),
            upperRight.north())};

    for (const auto& node : rootNodes) {
        if (node && node->extent.intersects(rectangle.rectangle)) {
            TileRectangleAvailability::putRectangleInQuadtree(
                *node,
                schemeId,
                maximumLevel,
                rectangle);
        }
    }
}

uint32_t QuantizedMeshTerrainProvider::TileRectangleAvailability::
    maximumLevelAtTileCenter(const TileKey& key) const {
    if (rootNodes.empty()) return 0;
    const Rectangle rectangle = TileRectangleAvailability::availabilityTileRectangle(
        schemeId,
        key.z,
        static_cast<uint32_t>(key.x),
        static_cast<uint32_t>(key.y));
    const auto center = rectangle.center();
    for (const auto& node : rootNodes) {
        if (node && node->extent.contains(center.first, center.second)) {
            return TileRectangleAvailability::findMaxLevelFromNode(
                nullptr,
                *node,
                center.first,
                center.second);
        }
    }
    return 0;
}

void QuantizedMeshTerrainProvider::setPlatformBridge(PlatformBridge* bridge) {
    platformBridge_ = bridge;
}

void QuantizedMeshTerrainProvider::setRequestHeaders(
    std::vector<HttpRequestOptions::Header> headers) {
    requestHeaders_ = std::move(headers);
}

void QuantizedMeshTerrainProvider::setZoomRange(int minZ, int maxZ) {
    minZoom_ = std::max(0, minZ);
    maxZoom_ = std::max(0, maxZ);
    if (layers_.size() == 1 && layers_.front().fallbackLayer) {
        layers_.front().minZoom = minZoom_;
        layers_.front().maxZoom = maxZoom_;
        layers_.front().contentAvailability.reset(
            layers_.front().schemeId,
            layers_.front().maxZoom);
    }
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

Rectangle webMercatorRectangle(const TileKey& key) {
    WebMercatorProjection projection(Ellipsoid::WGS84());
    const Rectangle projectedRoot =
        WebMercatorProjection::computeMaximumProjectedRectangle(
            Ellipsoid::WGS84());
    const double tilesAtZ = std::ldexp(1.0, key.z);
    const double tileWidth = projectedRoot.width() / tilesAtZ;
    const double tileHeight = projectedRoot.height() / tilesAtZ;
    const double west =
        projectedRoot.west() + static_cast<double>(key.x) * tileWidth;
    const double east = west + tileWidth;
    const double north =
        projectedRoot.north() - static_cast<double>(key.y) * tileHeight;
    const double south = north - tileHeight;
    return unprojectRectangleSimple(
        Projection(projection),
        Rectangle(west, south, east, north));
}

Rectangle terrainContentRectangle(const TileKey& key,
                                  const std::string& schemeId) {
    if (schemeId == "XYZ-WebMercator") {
        return webMercatorRectangle(key);
    }
    return geographicTmsRectangle(key);
}

RasterOverlayProjection rasterOverlayProjectionForTerrainScheme(
    const std::string& schemeId) {
    if (schemeId == "XYZ-WebMercator") {
        return RasterOverlayProjection::WebMercator;
    }
    return RasterOverlayProjection::Geographic;
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

std::vector<TileKey> quadtreeChildren(const TileKey& key) {
    const int childZ = key.z + 1;
    const int childX = key.x * 2;
    const int childY = key.y * 2;
    std::vector<TileKey> children;
    children.reserve(4);
    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            TileKey childKey{key.schemeId, childZ, childX + dx, childY + dy};
            if (isTileInLayerRange(childKey, key.schemeId)) {
                children.push_back(childKey);
            }
        }
    }
    return children;
}

} // namespace

int QuantizedMeshTerrainProvider::TileRectangleAvailability::rootTileCountX(const std::string& schemeId) {
    return schemeId == "Geographic-TMS" ? 2 : 1;
}

int QuantizedMeshTerrainProvider::TileRectangleAvailability::rootTileCountY(const std::string&) {
    return 1;
}

int QuantizedMeshTerrainProvider::TileRectangleAvailability::tileCountXAtLevel(const std::string& schemeId, int level) {
    return TileRectangleAvailability::rootTileCountX(schemeId) << level;
}

int QuantizedMeshTerrainProvider::TileRectangleAvailability::tileCountYAtLevel(const std::string& schemeId, int level) {
    return TileRectangleAvailability::rootTileCountY(schemeId) << level;
}

Rectangle QuantizedMeshTerrainProvider::TileRectangleAvailability::availabilityTileRectangle(const std::string& schemeId,
                                    int level,
                                    uint32_t x,
                                    uint32_t y) {
    const double xTiles = static_cast<double>(
        TileRectangleAvailability::tileCountXAtLevel(schemeId, level));
    const double yTiles = static_cast<double>(
        TileRectangleAvailability::tileCountYAtLevel(schemeId, level));
    const double tileX = static_cast<double>(x);
    const double tileY = static_cast<double>(y);
    return Rectangle(
        tileX / xTiles,
        tileY / yTiles,
        (tileX + 1.0) / xTiles,
        (tileY + 1.0) / yTiles);
}

bool QuantizedMeshTerrainProvider::TileRectangleAvailability::rectangleFullyContains(const Rectangle& outer, const Rectangle& inner) {
    return outer.west() <= inner.west() &&
           outer.south() <= inner.south() &&
           outer.east() >= inner.east() &&
           outer.north() >= inner.north();
}

void QuantizedMeshTerrainProvider::TileRectangleAvailability::reparentNodeChildren(
    QuantizedMeshTerrainProvider::TileRectangleAvailability::Node& node) {
    if (node.ll) node.ll->parent = &node;
    if (node.lr) node.lr->parent = &node;
    if (node.ul) node.ul->parent = &node;
    if (node.ur) node.ur->parent = &node;
}

std::unique_ptr<QuantizedMeshTerrainProvider::TileRectangleAvailability::Node>
QuantizedMeshTerrainProvider::TileRectangleAvailability::cloneNode(
    const QuantizedMeshTerrainProvider::TileRectangleAvailability::Node& node,
    QuantizedMeshTerrainProvider::TileRectangleAvailability::Node* parent) {
    auto clone = std::make_unique<
        QuantizedMeshTerrainProvider::TileRectangleAvailability::Node>(
        node.key,
        node.extent,
        parent);
    clone->rectangles = node.rectangles;
    if (node.ll) clone->ll = TileRectangleAvailability::cloneNode(*node.ll, clone.get());
    if (node.lr) clone->lr = TileRectangleAvailability::cloneNode(*node.lr, clone.get());
    if (node.ul) clone->ul = TileRectangleAvailability::cloneNode(*node.ul, clone.get());
    if (node.ur) clone->ur = TileRectangleAvailability::cloneNode(*node.ur, clone.get());
    return clone;
}

void QuantizedMeshTerrainProvider::TileRectangleAvailability::createNodeChildrenIfNecessary(
    QuantizedMeshTerrainProvider::TileRectangleAvailability::Node& node,
    const std::string& schemeId) {
    if (node.ll) return;

    const int childLevel = node.key.z + 1;
    const uint32_t childX = static_cast<uint32_t>(node.key.x * 2);
    const uint32_t childY = static_cast<uint32_t>(node.key.y * 2);
    const TileKey llKey{schemeId,
                        childLevel,
                        static_cast<int>(childX),
                        static_cast<int>(childY)};
    node.ll = std::make_unique<
        QuantizedMeshTerrainProvider::TileRectangleAvailability::Node>(
        llKey,
        TileRectangleAvailability::availabilityTileRectangle(schemeId, childLevel, childX, childY),
        &node);

    const TileKey lrKey{schemeId,
                        childLevel,
                        static_cast<int>(childX + 1),
                        static_cast<int>(childY)};
    node.lr = std::make_unique<
        QuantizedMeshTerrainProvider::TileRectangleAvailability::Node>(
        lrKey,
        TileRectangleAvailability::availabilityTileRectangle(schemeId, childLevel, childX + 1, childY),
        &node);

    const TileKey ulKey{schemeId,
                        childLevel,
                        static_cast<int>(childX),
                        static_cast<int>(childY + 1)};
    node.ul = std::make_unique<
        QuantizedMeshTerrainProvider::TileRectangleAvailability::Node>(
        ulKey,
        TileRectangleAvailability::availabilityTileRectangle(schemeId, childLevel, childX, childY + 1),
        &node);

    const TileKey urKey{schemeId,
                        childLevel,
                        static_cast<int>(childX + 1),
                        static_cast<int>(childY + 1)};
    node.ur = std::make_unique<
        QuantizedMeshTerrainProvider::TileRectangleAvailability::Node>(
        urKey,
        TileRectangleAvailability::availabilityTileRectangle(schemeId, childLevel, childX + 1, childY + 1),
        &node);
}

void QuantizedMeshTerrainProvider::TileRectangleAvailability::putRectangleInQuadtree(
    QuantizedMeshTerrainProvider::TileRectangleAvailability::Node& node,
    const std::string& schemeId,
    int maximumLevel,
    const QuantizedMeshTerrainProvider::TileRectangleAvailability::
        RectangleWithLevel& rectangle) {
    auto* current = &node;

    while (current->key.z < maximumLevel) {
        TileRectangleAvailability::createNodeChildrenIfNecessary(*current, schemeId);
        if (current->ll &&
            TileRectangleAvailability::rectangleFullyContains(current->ll->extent, rectangle.rectangle)) {
            current = current->ll.get();
        } else if (
            current->lr &&
            TileRectangleAvailability::rectangleFullyContains(current->lr->extent, rectangle.rectangle)) {
            current = current->lr.get();
        } else if (
            current->ul &&
            TileRectangleAvailability::rectangleFullyContains(current->ul->extent, rectangle.rectangle)) {
            current = current->ul.get();
        } else if (
            current->ur &&
            TileRectangleAvailability::rectangleFullyContains(current->ur->extent, rectangle.rectangle)) {
            current = current->ur.get();
        } else {
            break;
        }
    }

    auto& rectangles = current->rectangles;
    rectangles.push_back(rectangle);
    std::sort(
        rectangles.begin(),
        rectangles.end(),
        [](const auto& a, const auto& b) { return a.level < b.level; });
}

uint32_t QuantizedMeshTerrainProvider::TileRectangleAvailability::findMaxLevelFromNode(
    QuantizedMeshTerrainProvider::TileRectangleAvailability::Node* stopNode,
    QuantizedMeshTerrainProvider::TileRectangleAvailability::Node& node,
    double x,
    double y) {
    uint32_t maxLevel = 0;
    auto* current = &node;

    while (true) {
        const bool ll =
            current->ll && current->ll->extent.contains(x, y);
        const bool lr =
            current->lr && current->lr->extent.contains(x, y);
        const bool ul =
            current->ul && current->ul->extent.contains(x, y);
        const bool ur =
            current->ur && current->ur->extent.contains(x, y);
        const int containingChildren =
            (ll ? 1 : 0) + (lr ? 1 : 0) + (ul ? 1 : 0) + (ur ? 1 : 0);

        if (containingChildren > 1) {
            if (ll) {
                maxLevel = std::max(
                    maxLevel,
                    TileRectangleAvailability::findMaxLevelFromNode(
                        current,
                        *current->ll,
                        x,
                        y));
            }
            if (lr) {
                maxLevel = std::max(
                    maxLevel,
                    TileRectangleAvailability::findMaxLevelFromNode(
                        current,
                        *current->lr,
                        x,
                        y));
            }
            if (ul) {
                maxLevel = std::max(
                    maxLevel,
                    TileRectangleAvailability::findMaxLevelFromNode(
                        current,
                        *current->ul,
                        x,
                        y));
            }
            if (ur) {
                maxLevel = std::max(
                    maxLevel,
                    TileRectangleAvailability::findMaxLevelFromNode(
                        current,
                        *current->ur,
                        x,
                        y));
            }
            break;
        }

        if (ll) {
            current = current->ll.get();
        } else if (lr) {
            current = current->lr.get();
        } else if (ul) {
            current = current->ul.get();
        } else if (ur) {
            current = current->ur.get();
        } else {
            break;
        }
    }

    while (current != stopNode) {
        const auto& rectangles = current->rectangles;
        for (auto it = rectangles.rbegin();
             it != rectangles.rend() && it->level > maxLevel;
             ++it) {
            if (it->rectangle.contains(x, y)) {
                maxLevel = it->level;
            }
        }
        current = current->parent;
    }

    return maxLevel;
}

namespace {

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
    ParsedUrl parsed = parseUrl(url);
    if (!parsed.hasQuery) {
        parsed.hasQuery = true;
        parsed.query = assignment;
        url = composeUrl(parsed);
        return;
    }

    size_t pos = 0;
    bool found = false;
    while (pos <= parsed.query.size()) {
        const size_t valueStart = parsed.query.find('=', pos);
        const size_t paramEnd = parsed.query.find('&', pos);
        const size_t keyEnd =
            valueStart == std::string::npos ||
                    (paramEnd != std::string::npos && valueStart > paramEnd)
                ? paramEnd
                : valueStart;
        const size_t compareEnd =
            keyEnd == std::string::npos ? parsed.query.size() : keyEnd;
        if (parsed.query.compare(pos, compareEnd - pos, name) == 0) {
            found = true;
            break;
        }
        if (paramEnd == std::string::npos) {
            break;
        }
        pos = paramEnd + 1;
    }
    if (!found) {
        if (!parsed.query.empty()) parsed.query += "&";
        parsed.query += assignment;
        url = composeUrl(parsed);
        return;
    }

    size_t valueEnd = parsed.query.find('&', pos);
    if (valueEnd == std::string::npos) valueEnd = parsed.query.size();
    parsed.query.replace(pos, valueEnd - pos, assignment);

    pos += assignment.size();
    while (pos < parsed.query.size()) {
        const size_t partStart = parsed.query[pos] == '&' ? pos + 1 : pos;
        const size_t partEnd = parsed.query.find('&', partStart);
        const size_t end =
            partEnd == std::string::npos ? parsed.query.size() : partEnd;
        const size_t equals = parsed.query.find('=', partStart);
        const size_t keyEnd =
            equals == std::string::npos || equals > end ? end : equals;
        if (parsed.query.compare(partStart, keyEnd - partStart, name) == 0) {
            parsed.query.erase(pos, end - pos);
            continue;
        }
        pos = end;
    }
    url = composeUrl(parsed);
}

} // namespace

bool QuantizedMeshTerrainProvider::configureFromLayerJsonUrl(
    const std::string& layerJsonUrl) {
    QuantizedMeshLayerJsonFetcher fetcher(platformBridge_);
    auto bytes = fetcher.fetchBlocking(
        layerJsonUrl,
        HttpRequestPriority::Normal,
        requestHeaders_);
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
    layer.contentAvailability.reset(layer.schemeId, layer.maxZoom);
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
                const TileAvailabilityRect availabilityRange{
                    jsonUint32OrDefault(range, "startX"),
                    jsonUint32OrDefault(range, "startY"),
                    jsonUint32OrDefault(range, "endX"),
                    jsonUint32OrDefault(range, "endY")
                };
                layer.availabilityRanges[static_cast<size_t>(availabilityLevel)]
                    .push_back(availabilityRange);
                layer.contentAvailability.addAvailableTileRange(
                    availabilityLevel,
                    availabilityRange);
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
    auto bytes = fetcher.fetchBlocking(
        resolvedUrl,
        HttpRequestPriority::Normal,
        requestHeaders_);
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

std::vector<TileKey> QuantizedMeshTerrainProvider::rootTiles() const {
    return {
        TileSelectionRootPolicy::virtualTerrainRootKey(schemeId_)
    };
}

std::optional<TilesetContentTileMetadata>
QuantizedMeshTerrainProvider::tileMetadata(const TileKey& key) const {
    if (TileSelectionRootPolicy::isVirtualTerrainRoot(key) &&
        key.schemeId == schemeId_) {
        TilesetContentTileMetadata metadata;
        metadata.key = key;
        metadata.bounds = Rectangle::MAXIMUM;
        metadata.hasExplicitBounds = true;
        metadata.boundingVolume = TileBoundingVolume::fromRegion(
            metadata.bounds,
            kLooseTerrainMinimumHeight,
            kLooseTerrainMaximumHeight);
        metadata.contentBoundingVolume = metadata.boundingVolume;
        metadata.geometricError = calcLayerJsonTerrainGeometricError(
            Ellipsoid::WGS84(),
            metadata.bounds);
        metadata.refine = TileRefine::Replace;
        metadata.unconditionallyRefine = true;
        return metadata;
    }

    if (!isTileInLayerRange(key, schemeId_)) {
        return std::nullopt;
    }

    TilesetContentTileMetadata metadata;
    metadata.key = key;
    if (key.z == 0) {
        metadata.parentKey =
            TileSelectionRootPolicy::virtualTerrainRootKey(schemeId_);
    } else {
        metadata.parentKey =
            TileKey{schemeId_, key.z - 1, key.x / 2, key.y / 2};
    }
    metadata.bounds = terrainContentRectangle(key, schemeId_);
    metadata.hasExplicitBounds = true;
    metadata.boundingVolume = TileBoundingVolume::fromRegion(
        metadata.bounds,
        kLooseTerrainMinimumHeight,
        kLooseTerrainMaximumHeight);
    metadata.geometricError = calcLayerJsonTerrainGeometricError(
        Ellipsoid::WGS84(),
        metadata.bounds);
    metadata.refine = TileRefine::Replace;
    metadata.unconditionallyRefine = false;
    return metadata;
}

std::vector<TileKey>
QuantizedMeshTerrainProvider::childTiles(const TileKey& key) const {
    if (TileSelectionRootPolicy::isVirtualTerrainRoot(key) &&
        key.schemeId == schemeId_) {
        return TileSelectionRootPolicy::levelZeroTerrainRoots(schemeId_);
    }
    if (!isTileInLayerRange(key, schemeId_) || key.z >= maxZoom_) {
        return {};
    }
    return quadtreeChildren(key);
}

void QuantizedMeshTerrainProvider::resetFallbackLayerFromFields() {
    LayerConfig layer;
    layer.urlTemplate = urlTemplate_;
    layer.schemeId = schemeId_;
    layer.version = version_;
    layer.extensionsToRequest = extensionsToRequest_;
    layer.availabilityRanges = availabilityRanges_;
    layer.contentAvailability.reset(layer.schemeId, maxZoom_);
    layer.loadedSubtrees = loadedSubtrees_;
    layer.hasAvailability = hasAvailability_;
    layer.availabilityLevels = availabilityLevels_;
    layer.minZoom = minZoom_;
    layer.maxZoom = maxZoom_;
    layer.attribution = attribution_;
    layer.fallbackLayer = true;
    layers_.clear();
    layers_.push_back(std::move(layer));
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
    if (layer.fallbackLayer &&
        (key.z < layer.minZoom || key.z > layer.maxZoom)) {
        return TileAvailabilityState::NotAvailable;
    }
    if (!isTileInLayerRange(key, layer.schemeId)) {
        return TileAvailabilityState::NotAvailable;
    }
    if (!layer.hasAvailability) return TileAvailabilityState::Available;

    if (layer.contentAvailability.maximumLevelAtTileCenter(key) >=
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
    if (!layers_.empty() && firstAvailableLayerIndex(key) >= layers_.size()) {
        return 0;
    }
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
    const std::string level = std::to_string(key.z);
    std::string url = substituteTemplateParameters(
        layer.urlTemplate,
        [&key, &layer, &level](const std::string& placeholder) {
            if (placeholder == "level" || placeholder == "z") {
                return level;
            }
            if (placeholder == "x") {
                return std::to_string(key.x);
            }
            if (placeholder == "y") {
                return std::to_string(key.y);
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

    return {};
}

void QuantizedMeshTerrainProvider::requestTileContent(
    const TileKey& key,
    CancellationToken token,
    ContentCallback callback,
    HttpRequestPriority priority) {
    const size_t contentLayerIndexValue = firstAvailableLayerIndex(key);
    const bool hasContentLayer =
        contentLayerIndexValue < layers_.size();
    const int contentLayerIndex = hasContentLayer
        ? static_cast<int>(contentLayerIndexValue)
        : -1;
    const LayerConfig* contentLayer =
        hasContentLayer ? &layers_[contentLayerIndexValue] : nullptr;
    const bool includeCurrentLayerMetadata =
        contentLayer && contentLayer->availabilityLevels >= 1 &&
        key.z % contentLayer->availabilityLevels == 0 &&
        !isSubtreeLoadedInLayer(
            *contentLayer,
            key.z / contentLayer->availabilityLevels,
            mortonEncode2D(static_cast<uint32_t>(key.x),
                           static_cast<uint32_t>(key.y)));
    std::string url = contentLayer
        ? buildUrlForLayer(*contentLayer, key)
        : buildUrl(key);
    std::vector<LayerAvailabilityRequest> availabilityRequests =
        collectUnderlyingLayerAvailabilityRequests(key);
    auto availabilityRequestsPtr =
        std::make_shared<std::vector<LayerAvailabilityRequest>>(
            std::move(availabilityRequests));
    auto tokenPtr = std::make_shared<CancellationToken>(std::move(token));
    auto callbackPtr =
        std::make_shared<ContentCallback>(std::move(callback));
    auto requestState = std::make_shared<AsyncTileRequestState>();
    if (!layers_.empty() && !contentLayer) {
        requestsStarted_.fetch_add(1, std::memory_order_relaxed);
        requestsCompleted_.fetch_add(1, std::memory_order_relaxed);
        (*callbackPtr)(key, TileContentLoadResult::failed());
        return;
    }
    if (platformBridge_) {
        requestsStarted_.fetch_add(1, std::memory_order_relaxed);
        if (tokenPtr->isCancelled()) {
            requestsCompleted_.fetch_add(1, std::memory_order_relaxed);
            (*callbackPtr)(key, TileContentLoadResult::cancelled());
            return;
        }

        auto requestHandle =
            std::make_shared<std::unique_ptr<HttpRequest>>();
        *requestHandle = platformBridge_->get(
            url,
            [this,
             key,
             contentLayerIndex,
             includeCurrentLayerMetadata,
             availabilityRequestsPtr,
             tokenPtr,
             callbackPtr,
             requestState,
             priority,
             requestHandle](int statusCode, std::vector<uint8_t> body) mutable {
                (void)requestHandle;
                handleAsyncTileBody(
                    key,
                    contentLayerIndex,
                    includeCurrentLayerMetadata,
                    availabilityRequestsPtr,
                    tokenPtr,
                    callbackPtr,
                    requestState,
                    priority,
                    statusCode,
                    std::move(body));
            },
            {priority, requestHeaders_});
        startAsyncMetadataRequests(
            key,
            contentLayerIndex,
            includeCurrentLayerMetadata,
            availabilityRequestsPtr,
            tokenPtr,
            callbackPtr,
            requestState,
            priority,
            true);
        return;
    }

    requestsStarted_.fetch_add(1, std::memory_order_relaxed);
    if (tokenPtr->isCancelled()) {
        requestsCompleted_.fetch_add(1, std::memory_order_relaxed);
        (*callbackPtr)(key, TileContentLoadResult::cancelled());
        return;
    }

    if (isFileUrl(url)) {
        startAsyncMetadataRequests(
            key,
            contentLayerIndex,
            includeCurrentLayerMetadata,
            availabilityRequestsPtr,
            tokenPtr,
            callbackPtr,
            requestState,
            priority,
            false);
        AsyncSystem::pool().enqueue(
            [this,
             url,
             key,
             contentLayerIndex,
             includeCurrentLayerMetadata,
             availabilityRequestsPtr,
             tokenPtr,
             callbackPtr,
             requestState,
             priority]() mutable {
                if (tokenPtr->isCancelled()) {
                    handleAsyncTileBody(
                        key,
                        contentLayerIndex,
                        includeCurrentLayerMetadata,
                        availabilityRequestsPtr,
                        tokenPtr,
                        callbackPtr,
                        requestState,
                        priority,
                        0,
                        {});
                    return;
                }

                std::vector<uint8_t> body = readFileUrl(url);
#ifdef __ANDROID__
                __android_log_print(
                    ANDROID_LOG_INFO,
                    "QMTerrain",
                    "requestTile: z=%d x=%d y=%d body=%zu canceled=%d",
                    key.z,
                    key.x,
                    key.y,
                    body.size(),
                    tokenPtr->isCancelled());
#endif
                handleAsyncTileBody(
                    key,
                    contentLayerIndex,
                    includeCurrentLayerMetadata,
                    availabilityRequestsPtr,
                    tokenPtr,
                    callbackPtr,
                    requestState,
                    priority,
                    body.empty() ? 0 : 200,
                    std::move(body));
            });
        return;
    }

    auto requestHandle =
        std::make_shared<std::unique_ptr<HttpRequest>>();
    *requestHandle = CurlMultiRequestScheduler::shared().get(
        url,
        [this,
         key,
         contentLayerIndex,
         includeCurrentLayerMetadata,
         availabilityRequestsPtr,
         tokenPtr,
         callbackPtr,
         requestState,
         priority,
         requestHandle](int statusCode, std::vector<uint8_t> body) mutable {
            (void)requestHandle;
            handleAsyncTileBody(
                key,
                contentLayerIndex,
                includeCurrentLayerMetadata,
                availabilityRequestsPtr,
                tokenPtr,
                callbackPtr,
                requestState,
                priority,
                statusCode,
                std::move(body));
        },
        {priority, requestHeaders_});
    startAsyncMetadataRequests(
        key,
        contentLayerIndex,
        includeCurrentLayerMetadata,
        availabilityRequestsPtr,
        tokenPtr,
        callbackPtr,
        requestState,
        priority,
        false);
}

void QuantizedMeshTerrainProvider::handleAsyncTileBody(
    const TileKey& key,
    int contentLayerIndex,
    bool includeCurrentLayerMetadata,
    std::shared_ptr<std::vector<LayerAvailabilityRequest>>
        availabilityRequests,
    std::shared_ptr<CancellationToken> token,
    std::shared_ptr<ContentCallback> callback,
    std::shared_ptr<AsyncTileRequestState> state,
    HttpRequestPriority priority,
    int statusCode,
    std::vector<uint8_t> body) {
    (void)priority;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->statusCode = statusCode;
        state->body = std::move(body);
        state->contentFinished = true;
    }
    completeAsyncTileRequestIfReady(
        key,
        contentLayerIndex,
        includeCurrentLayerMetadata,
        availabilityRequests,
        token,
        callback,
        state);
}

void QuantizedMeshTerrainProvider::startAsyncMetadataRequests(
    TileKey key,
    int contentLayerIndex,
    bool includeCurrentLayerMetadata,
    std::shared_ptr<std::vector<LayerAvailabilityRequest>>
        availabilityRequests,
    std::shared_ptr<CancellationToken> token,
    std::shared_ptr<ContentCallback> callback,
    std::shared_ptr<AsyncTileRequestState> state,
    HttpRequestPriority priority,
    bool usePlatformBridge) {
    if (availabilityRequests->empty()) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->metadataFinished = true;
        }
        completeAsyncTileRequestIfReady(
            key,
            contentLayerIndex,
            includeCurrentLayerMetadata,
            availabilityRequests,
            token,
            callback,
            state);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->metadataBodies.resize(availabilityRequests->size());
        state->metadataHandles.resize(availabilityRequests->size());
        state->remainingMetadata = availabilityRequests->size();
    }

    for (size_t i = 0; i < availabilityRequests->size(); ++i) {
        requestsStarted_.fetch_add(1, std::memory_order_relaxed);
        auto metadataCallback =
            [this,
             state,
             key,
             contentLayerIndex,
             includeCurrentLayerMetadata,
             availabilityRequests,
             token,
             callback,
             i](int metadataStatusCode,
                std::vector<uint8_t> metadataBody) mutable {
                requestsCompleted_.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (!state->finalized &&
                        !token->isCancelled() &&
                        isCesiumSuccessfulHttpStatus(metadataStatusCode) &&
                        !metadataBody.empty()) {
                        state->metadataBodies[i] = std::move(metadataBody);
                    }
                    if (state->remainingMetadata > 0) {
                        --state->remainingMetadata;
                    }
                    if (state->remainingMetadata == 0) {
                        state->metadataFinished = true;
                    }
                }
                completeAsyncTileRequestIfReady(
                    key,
                    contentLayerIndex,
                    includeCurrentLayerMetadata,
                    availabilityRequests,
                    token,
                    callback,
                    state);
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
            state->metadataHandles[i] = platformBridge_->get(
                metadataUrl,
                std::move(metadataCallback),
                {priority, requestHeaders_});
        } else {
            state->metadataHandles[i] =
                CurlMultiRequestScheduler::shared().get(
                    metadataUrl,
                    std::move(metadataCallback),
                    {priority, requestHeaders_});
        }
    }
}

void QuantizedMeshTerrainProvider::completeAsyncTileRequestIfReady(
    TileKey key,
    int contentLayerIndex,
    bool includeCurrentLayerMetadata,
    std::shared_ptr<std::vector<LayerAvailabilityRequest>>
        availabilityRequests,
    std::shared_ptr<CancellationToken> token,
    std::shared_ptr<ContentCallback> callback,
    std::shared_ptr<AsyncTileRequestState> state) {
    int statusCode = 0;
    std::vector<uint8_t> body;
    std::vector<std::vector<uint8_t>> metadataBodies;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->finalized ||
            !state->contentFinished ||
            !state->metadataFinished) {
            return;
        }
        state->finalized = true;
        statusCode = state->statusCode;
        body = std::move(state->body);
        metadataBodies = std::move(state->metadataBodies);
    }

    auto bodyPtr =
        std::make_shared<std::vector<uint8_t>>(std::move(body));
    finalizeAsyncTileRequest(
        key,
        contentLayerIndex,
        includeCurrentLayerMetadata,
        availabilityRequests,
        token,
        callback,
        bodyPtr,
        statusCode,
        std::move(metadataBodies));
}

void QuantizedMeshTerrainProvider::finalizeAsyncTileRequest(
    TileKey key,
    int contentLayerIndex,
    bool includeCurrentLayerMetadata,
    std::shared_ptr<std::vector<LayerAvailabilityRequest>>
        availabilityRequests,
    std::shared_ptr<CancellationToken> token,
    std::shared_ptr<ContentCallback> callback,
    std::shared_ptr<std::vector<uint8_t>> body,
    int statusCode,
    std::vector<std::vector<uint8_t>> metadataBodies) {
    AsyncSystem::pool().enqueue(
        [this,
         key,
         contentLayerIndex,
         includeCurrentLayerMetadata,
         availabilityRequests,
         token,
         callback,
         body,
         statusCode,
         metadataBodies = std::move(metadataBodies)]() mutable {
            RequestCompletionGuard completion{requestsCompleted_};
            if (token->isCancelled()) {
                completion.complete();
                (*callback)(key, TileContentLoadResult::cancelled());
                return;
            }
            if (!isCesiumSuccessfulHttpStatus(statusCode) || body->empty()) {
                completion.complete();
                (*callback)(key, TileContentLoadResult::failed());
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
            std::vector<QuantizedMeshMetadataContent> metadata;
            metadata.reserve(availabilityRequests->size());
            std::optional<QuantizedMeshAvailabilityUpdate>
                currentTileAvailabilityUpdate;
            if (includeCurrentLayerMetadata && contentLayerIndex >= 0) {
                QuantizedMeshAvailabilityUpdate update;
                update.layerIndex = contentLayerIndex;
                update.subtreeKey = key;
                currentTileAvailabilityUpdate = std::move(update);
            }
            for (size_t i = 0; i < availabilityRequests->size(); ++i) {
                const LayerAvailabilityRequest& request =
                    (*availabilityRequests)[i];
                QuantizedMeshMetadataContent item;
                item.layerIndex = static_cast<int>(request.layerIndex);
                item.subtreeKey = request.subtreeKey;
                if (i < metadataBodies.size() &&
                    !metadataBodies[i].empty()) {
                    item.data = metadataBodies[i].data();
                    item.size = metadataBodies[i].size();
                }
                metadata.push_back(item);
            }

            const std::string contentSchemeId =
                contentLayerIndex >= 0 &&
                        static_cast<size_t>(contentLayerIndex) < layers_.size()
                    ? layers_[static_cast<size_t>(contentLayerIndex)].schemeId
                    : schemeId_;
            QuantizedMeshContentLoadResult contentResult =
                QuantizedMeshContentLoader::load(
                    body->data(),
                    body->size(),
                    terrainContentRectangle(key, contentSchemeId),
                    waterMaskEnabled_,
                    metadata,
                    rasterOverlayProjectionForTerrainScheme(contentSchemeId),
                    std::move(currentTileAvailabilityUpdate));
            if (!contentResult.success()) {
                completion.complete();
                (*callback)(key, TileContentLoadResult::failed());
                return;
            }

            TileContentLoadResult result =
                TileContentLoadResult::render(
                    std::move(contentResult.gltfModel));
            result.terrainRenderContent = result.gltfModel != nullptr;
            result.metadata = std::move(contentResult.metadata);
            result.quantizedMeshAvailabilityUpdates =
                std::move(contentResult.availabilityUpdates);
            completion.complete();
            (*callback)(key, std::move(result));
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
    int level, const std::vector<TileAvailabilityRect>& rects) {
    if (level < 0) return;
    if (layers_.empty()) return;
    addAvailabilityRectsToLayer(layers_.front(), level, rects);
    syncLegacyFieldsFromPrimaryLayer();
}

void QuantizedMeshTerrainProvider::addAvailabilityRectsToLayer(
    LayerConfig& layer,
    int level,
    const std::vector<TileAvailabilityRect>& rects) {
    if (level < 0) return;
    layer.hasAvailability = true;
    if (static_cast<size_t>(level) >= layer.availabilityRanges.size()) {
        layer.availabilityRanges.resize(static_cast<size_t>(level) + 1);
    }
    auto& ranges = layer.availabilityRanges[static_cast<size_t>(level)];
    for (const auto& r : rects) {
        ranges.push_back(r);
        layer.contentAvailability.addAvailableTileRange(level, r);
    }
}

void QuantizedMeshTerrainProvider::addAvailabilityRectsForTile(
    const TileKey& subtreeKey,
    int level,
    const std::vector<TileAvailabilityRect>& rects) {
    LayerConfig* layer = firstAvailableLayer(subtreeKey);
    if (!layer || level < 0) return;
    addAvailabilityRectsToLayer(*layer, level, rects);
    syncLegacyFieldsFromPrimaryLayer();
}

bool QuantizedMeshTerrainProvider::isSubtreeLoaded(
    int subtreeLevel, uint64_t mortonIndex) const {
    if (layers_.empty()) return false;
    return isSubtreeLoadedInLayer(
        layers_.front(), subtreeLevel, mortonIndex);
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
    if (layers_.empty()) return;
    markSubtreeLoadedInLayer(layers_.front(), subtreeLevel, mortonIndex);
    syncLegacyFieldsFromPrimaryLayer();
}

void QuantizedMeshTerrainProvider::markSubtreeLoadedForTile(
    const TileKey& subtreeKey) {
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
    const std::vector<QuantizedMeshAvailabilityUpdate>& updates) {
    if (updates.empty()) {
        return;
    }

    for (const auto& update : updates) {
        if (update.layerIndex < 0 ||
            static_cast<size_t>(update.layerIndex) >= layers_.size()) {
            continue;
        }

        LayerConfig& layer = layers_[static_cast<size_t>(update.layerIndex)];
        if (layer.availabilityLevels <= 0) {
            continue;
        }
        for (const auto& r : update.metadataAvailability) {
            const int absLevel =
                update.subtreeKey.z + 1 + static_cast<int>(r[0]);
            if (absLevel >= 0) {
                addAvailabilityRectsToLayer(
                    layer, absLevel, {{r[1], r[2], r[3], r[4]}});
            }
        }

        markSubtreeLoadedInLayer(
            layer,
            update.subtreeKey.z / layer.availabilityLevels,
            mortonEncode2D(static_cast<uint32_t>(update.subtreeKey.x),
                           static_cast<uint32_t>(update.subtreeKey.y)));
    }
    syncLegacyFieldsFromPrimaryLayer();
}

bool QuantizedMeshTerrainProvider::isAvailabilityBoundaryLevel(int level) const {
    for (const LayerConfig& layer : layers_) {
        if (layer.availabilityLevels > 0 &&
            level % layer.availabilityLevels == 0) {
            return true;
        }
    }
    return false;
}

TileContentLoadResult QuantizedMeshTerrainProvider::decodeContent(
    const uint8_t*, size_t) {
    return TileContentLoadResult::failed();
}

} // namespace earth_engine
