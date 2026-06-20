#include "GoogleMapTilesImageryProvider.h"

#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace earth_engine {
namespace {

std::string escapeQueryValue(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('%');
            result.push_back(hex[c >> 4]);
            result.push_back(hex[c & 0x0f]);
        }
    }
    return result;
}

std::string ensureTrailingSlash(std::string url) {
    if (url.empty() || url.back() == '/') {
        return url;
    }
    return url + "/";
}

std::string withQuery(
    std::string url,
    const std::vector<std::pair<std::string, std::string>>& query) {
    std::string fragment;
    const size_t fragmentStart = url.find('#');
    if (fragmentStart != std::string::npos) {
        fragment = url.substr(fragmentStart);
        url.erase(fragmentStart);
    }

    url += url.find('?') == std::string::npos ? '?' : '&';
    for (size_t i = 0; i < query.size(); ++i) {
        if (i > 0) {
            url.push_back('&');
        }
        url += query[i].first;
        url.push_back('=');
        url += escapeQueryValue(query[i].second);
    }
    return url + fragment;
}

int invertedY(int level, int y) {
    if (level < 0 || level > 30) {
        return y;
    }
    return static_cast<int>((int64_t{1} << level) - 1 - y);
}

} // namespace

GoogleMapTilesImageryProvider::GoogleMapTilesImageryProvider(
    GoogleMapTilesExistingSessionOptions options,
    std::string attribution)
    : XYZImageryProvider(std::string(), std::move(attribution))
    , options_(std::move(options)) {
    options_.apiBaseUrl = ensureTrailingSlash(options_.apiBaseUrl);
    setSchemeId("XYZ-WebMercator");
    setZoomRange(0, options_.maximumLevel < 0 ? 0 : options_.maximumLevel);
    setTileSize(options_.tileWidth < 1 ? 1 : options_.tileWidth,
                options_.tileHeight < 1 ? 1 : options_.tileHeight);
}

std::string GoogleMapTilesImageryProvider::id() const {
    std::ostringstream oss;
    oss << "google-map-tiles-"
        << std::hash<std::string>{}(options_.apiBaseUrl + options_.session);
    return oss.str();
}

bool GoogleMapTilesImageryProvider::supportsTile(const TileKey& key) const {
    return XYZImageryProvider::supportsTile(key);
}

std::string GoogleMapTilesImageryProvider::buildUrl(
    const TileKey& key) const {
    if (!supportsTile(key)) {
        return std::string();
    }

    const std::string path =
        "v1/2dtiles/" + std::to_string(key.z) + "/" +
        std::to_string(key.x) + "/" +
        std::to_string(invertedY(key.z, key.y));
    return withQuery(
        options_.apiBaseUrl + path,
        {{"session", options_.session}, {"key", options_.key}});
}

} // namespace earth_engine
