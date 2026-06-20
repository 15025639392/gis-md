#include "WebMapServiceImageryProvider.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace earth_engine {
namespace {

struct QueryPart {
    std::string key;
    std::string value;
};

struct UrlParts {
    std::string prefix;
    std::vector<QueryPart> query;
    std::string fragment;
};

UrlParts splitUrl(std::string url) {
    UrlParts parts;
    const size_t fragmentStart = url.find('#');
    if (fragmentStart != std::string::npos) {
        parts.fragment = url.substr(fragmentStart);
        url.erase(fragmentStart);
    }

    const size_t queryStart = url.find('?');
    if (queryStart == std::string::npos) {
        parts.prefix = std::move(url);
        return parts;
    }

    parts.prefix = url.substr(0, queryStart);
    const std::string queryString = url.substr(queryStart + 1);
    size_t start = 0;
    while (start <= queryString.size()) {
        const size_t end = queryString.find('&', start);
        const std::string pair = queryString.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        if (!pair.empty()) {
            const size_t equals = pair.find('=');
            if (equals == std::string::npos) {
                parts.query.push_back(QueryPart{pair, std::string()});
            } else {
                parts.query.push_back(QueryPart{
                    pair.substr(0, equals),
                    pair.substr(equals + 1)});
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return parts;
}

void setQueryValue(std::vector<QueryPart>& query,
                   const std::string& key,
                   std::string value,
                   bool overwrite) {
    auto it = std::find_if(query.begin(), query.end(), [&](const QueryPart& p) {
        return p.key == key;
    });
    if (it == query.end()) {
        query.push_back(QueryPart{key, std::move(value)});
    } else if (overwrite) {
        it->value = std::move(value);
    }
}

std::string joinUrl(const UrlParts& parts) {
    std::string result = parts.prefix;
    if (!parts.query.empty()) {
        result.push_back('?');
        for (size_t i = 0; i < parts.query.size(); ++i) {
            if (i > 0) result.push_back('&');
            result += parts.query[i].key;
            result.push_back('=');
            result += parts.query[i].value;
        }
    }
    result += parts.fragment;
    return result;
}

} // namespace

WebMapServiceImageryProvider::WebMapServiceImageryProvider(
    std::string baseUrl,
    WebMapServiceImageryOptions options,
    std::string attribution)
    : XYZImageryProvider(std::string(), std::move(attribution))
    , baseUrl_(std::move(baseUrl))
    , options_(std::move(options)) {
    setSchemeId("Geographic-TMS");
    setZoomRange(options_.minimumLevel < 0 ? 0 : options_.minimumLevel,
                 options_.maximumLevel < 0 ? 0 : options_.maximumLevel);
    setTileSize(options_.tileWidth < 1 ? 1 : options_.tileWidth,
                options_.tileHeight < 1 ? 1 : options_.tileHeight);
}

std::string WebMapServiceImageryProvider::id() const {
    std::ostringstream oss;
    oss << "wms-" << std::hash<std::string>{}(baseUrl_);
    return oss.str();
}

bool WebMapServiceImageryProvider::supportsTile(const TileKey& key) const {
    return XYZImageryProvider::supportsTile(key);
}

std::string WebMapServiceImageryProvider::buildUrl(const TileKey& key) const {
    if (!supportsTile(key)) {
        return std::string();
    }

    const int64_t xTiles = int64_t{1} << (key.z + 1);
    const int64_t yTiles = int64_t{1} << key.z;
    const double west =
        static_cast<double>(key.x) / static_cast<double>(xTiles) * 360.0 -
        180.0;
    const double east =
        static_cast<double>(key.x + 1) / static_cast<double>(xTiles) * 360.0 -
        180.0;
    const double south =
        -90.0 +
        static_cast<double>(key.y) / static_cast<double>(yTiles) * 180.0;
    const double north =
        -90.0 +
        static_cast<double>(key.y + 1) / static_cast<double>(yTiles) * 180.0;

    UrlParts parts = splitUrl(baseUrl_);
    setQueryValue(parts.query, "crs", "EPSG:4326", false);
    setQueryValue(parts.query, "styles", "", false);
    setQueryValue(parts.query, "transparent", "true", false);
    setQueryValue(parts.query, "service", "WMS", false);

    setQueryValue(parts.query, "request", "GetMap", true);
    setQueryValue(parts.query, "version", options_.version, true);
    setQueryValue(parts.query,
                  "bbox",
                  std::to_string(south) + "," + std::to_string(west) + "," +
                      std::to_string(north) + "," + std::to_string(east),
                  true);
    setQueryValue(parts.query, "layers", options_.layers, true);
    setQueryValue(parts.query, "format", options_.format, true);
    setQueryValue(parts.query, "width", std::to_string(tileWidth()), true);
    setQueryValue(parts.query, "height", std::to_string(tileHeight()), true);

    return joinUrl(parts);
}

} // namespace earth_engine
