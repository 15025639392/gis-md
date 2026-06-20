#include "BingMapsImageryProvider.h"

#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <utility>

namespace earth_engine {
namespace {

bool isAbsoluteUrl(const std::string& url) {
    return url.find("://") != std::string::npos || url.rfind("//", 0) == 0;
}

std::string resolveUrl(const std::string& baseUrl,
                       const std::string& relativeOrAbsolute) {
    if (isAbsoluteUrl(relativeOrAbsolute) || baseUrl.empty()) {
        return relativeOrAbsolute;
    }

    if (!relativeOrAbsolute.empty() && relativeOrAbsolute.front() == '/') {
        const size_t scheme = baseUrl.find("://");
        if (scheme == std::string::npos) {
            return relativeOrAbsolute;
        }
        const size_t pathStart = baseUrl.find('/', scheme + 3);
        if (pathStart == std::string::npos) {
            return baseUrl + relativeOrAbsolute;
        }
        return baseUrl.substr(0, pathStart) + relativeOrAbsolute;
    }

    std::string prefix = baseUrl;
    const size_t fragment = prefix.find('#');
    if (fragment != std::string::npos) {
        prefix.erase(fragment);
    }
    const size_t query = prefix.find('?');
    if (query != std::string::npos) {
        prefix.erase(query);
    }

    if (!prefix.empty() && prefix.back() != '/') {
        const size_t slash = prefix.rfind('/');
        const size_t scheme = prefix.find("://");
        if (slash != std::string::npos &&
            (scheme == std::string::npos || slash > scheme + 2)) {
            prefix.erase(slash + 1);
        } else {
            prefix.push_back('/');
        }
    }
    return prefix + relativeOrAbsolute;
}

std::string replaceTemplateParameters(
    const std::string& templateUrl,
    const std::function<std::string(const std::string&)>& replacer) {
    std::string result;
    size_t start = 0;
    while (true) {
        const size_t open = templateUrl.find('{', start);
        if (open == std::string::npos) {
            break;
        }
        result.append(templateUrl, start, open - start);

        const size_t close = templateUrl.find('}', open + 1);
        if (close == std::string::npos) {
            start = open;
            break;
        }

        const std::string key = templateUrl.substr(open + 1, close - open - 1);
        result += replacer(key);
        start = close + 1;
    }
    result.append(templateUrl, start, templateUrl.size() - start);
    return result;
}

bool queryHasKey(const std::string& query, const std::string& key) {
    size_t start = 0;
    while (start <= query.size()) {
        const size_t end = query.find('&', start);
        const std::string part = query.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        const size_t equals = part.find('=');
        const std::string partKey =
            equals == std::string::npos ? part : part.substr(0, equals);
        if (partKey == key) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

std::string appendNParameterIfMissing(std::string url) {
    std::string fragment;
    const size_t fragmentStart = url.find('#');
    if (fragmentStart != std::string::npos) {
        fragment = url.substr(fragmentStart);
        url.erase(fragmentStart);
    }

    const size_t queryStart = url.find('?');
    if (queryStart != std::string::npos) {
        const std::string query = url.substr(queryStart + 1);
        if (queryHasKey(query, "n")) {
            return url + fragment;
        }
        url += query.empty() ? "n=z" : "&n=z";
        return url + fragment;
    }

    url += "?n=z";
    return url + fragment;
}

int invertedY(int level, int y) {
    if (level < 0 || level > 30) {
        return y;
    }
    return static_cast<int>((int64_t{1} << level) - 1 - y);
}

} // namespace

BingMapsImageryProvider::BingMapsImageryProvider(
    std::string baseUrl,
    std::string urlTemplate,
    BingMapsImageryOptions options,
    std::string attribution)
    : XYZImageryProvider(std::string(), std::move(attribution))
    , baseUrl_(std::move(baseUrl))
    , urlTemplate_(std::move(urlTemplate))
    , options_(std::move(options)) {
    setSchemeId("XYZ-WebMercator");
    setZoomRange(options_.minimumLevel < 0 ? 0 : options_.minimumLevel,
                 options_.maximumLevel < 0 ? 0 : options_.maximumLevel);
    setTileSize(options_.tileWidth < 1 ? 1 : options_.tileWidth,
                options_.tileHeight < 1 ? 1 : options_.tileHeight);
}

std::string BingMapsImageryProvider::id() const {
    std::ostringstream oss;
    oss << "bing-" << std::hash<std::string>{}(baseUrl_ + urlTemplate_);
    return oss.str();
}

bool BingMapsImageryProvider::supportsTile(const TileKey& key) const {
    return XYZImageryProvider::supportsTile(key);
}

std::string BingMapsImageryProvider::buildUrl(const TileKey& key) const {
    if (!supportsTile(key)) {
        return std::string();
    }

    const int level = key.z;
    const int x = key.x;
    const int y = key.y;
    const int bingY = invertedY(level, y);
    const std::string substituted = replaceTemplateParameters(
        urlTemplate_,
        [&](const std::string& placeholder) {
            if (placeholder == "quadkey") {
                return tileXYToQuadKey(level, x, bingY);
            }
            if (placeholder == "subdomain") {
                if (options_.subdomains.empty()) {
                    return std::string();
                }
                const size_t index =
                    static_cast<size_t>(level + x + y) %
                    options_.subdomains.size();
                return options_.subdomains[index];
            }
            if (placeholder == "culture") {
                return options_.culture;
            }
            return placeholder;
        });

    return appendNParameterIfMissing(resolveUrl(baseUrl_, substituted));
}

std::string BingMapsImageryProvider::tileXYToQuadKey(int level, int x, int y) {
    std::string quadkey;
    if (level < 0) {
        return quadkey;
    }
    for (int32_t i = static_cast<int32_t>(level); i >= 0; --i) {
        const uint32_t bitmask = static_cast<uint32_t>(1U << i);
        uint32_t digit = 0;
        if ((static_cast<uint32_t>(x) & bitmask) != 0) {
            digit |= 1;
        }
        if ((static_cast<uint32_t>(y) & bitmask) != 0) {
            digit |= 2;
        }
        quadkey += std::to_string(digit);
    }
    return quadkey;
}

} // namespace earth_engine
