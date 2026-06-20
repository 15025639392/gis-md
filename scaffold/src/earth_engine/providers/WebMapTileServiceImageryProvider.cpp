#include "WebMapTileServiceImageryProvider.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <utility>

namespace earth_engine {
namespace {

std::string escapeUriComponent(const std::string& value) {
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

std::string substituteTemplateParameters(
    const std::string& templateUrl,
    const std::map<std::string, std::string>& replacements) {
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

        const std::string placeholder =
            templateUrl.substr(open + 1, close - open - 1);
        const auto it = replacements.find(placeholder);
        if (it == replacements.end()) {
            result.push_back('{');
            result += placeholder;
            result.push_back('}');
        } else {
            result += escapeUriComponent(it->second);
        }
        start = close + 1;
    }
    result.append(templateUrl, start, templateUrl.size() - start);
    return result;
}

bool shouldUseKvp(const std::string& url) {
    const auto count = static_cast<int>(std::count(url.begin(), url.end(), '{'));
    return count < 1 || (count == 1 && url.find("{s}") != std::string::npos);
}

std::string tileMatrixLabel(const WebMapTileServiceImageryOptions& options,
                            int level) {
    if (options.tileMatrixLabels &&
        level >= 0 &&
        static_cast<size_t>(level) < options.tileMatrixLabels->size()) {
        return (*options.tileMatrixLabels)[static_cast<size_t>(level)];
    }
    return std::to_string(level);
}

int invertedY(int level, int y) {
    if (level < 0 || level >= 30) {
        return y;
    }
    return (1 << level) - 1 - y;
}

void addDimensions(std::map<std::string, std::string>& replacements,
                   const WebMapTileServiceImageryOptions& options) {
    if (options.dimensions) {
        replacements.insert(options.dimensions->begin(),
                            options.dimensions->end());
    }
}

void addSubdomain(std::map<std::string, std::string>& replacements,
                  const WebMapTileServiceImageryOptions& options,
                  int col,
                  int row,
                  int level) {
    if (!options.subdomains.empty()) {
        replacements.emplace(
            "s",
            options.subdomains[
                static_cast<size_t>(col + row + level) %
                options.subdomains.size()]);
    }
}

} // namespace

WebMapTileServiceImageryProvider::WebMapTileServiceImageryProvider(
    std::string url,
    WebMapTileServiceImageryOptions options,
    std::string attribution)
    : XYZImageryProvider(std::string(), std::move(attribution))
    , url_(std::move(url))
    , options_(std::move(options))
    , useKvp_(shouldUseKvp(url_)) {
    setSchemeId("XYZ-WebMercator");
    setZoomRange(options_.minimumLevel < 0 ? 0 : options_.minimumLevel,
                 options_.maximumLevel < 0 ? 0 : options_.maximumLevel);
    setTileSize(options_.tileWidth < 1 ? 1 : options_.tileWidth,
                options_.tileHeight < 1 ? 1 : options_.tileHeight);
}

std::string WebMapTileServiceImageryProvider::id() const {
    std::ostringstream oss;
    oss << "wmts-" << std::hash<std::string>{}(url_);
    return oss.str();
}

bool WebMapTileServiceImageryProvider::supportsTile(const TileKey& key) const {
    return XYZImageryProvider::supportsTile(key);
}

std::string WebMapTileServiceImageryProvider::buildUrl(
    const TileKey& key) const {
    if (!supportsTile(key)) {
        return std::string();
    }

    const int level = key.z;
    const int col = key.x;
    const int row = invertedY(level, key.y);
    const std::string matrix = tileMatrixLabel(options_, level);
    const std::string format = options_.format.value_or("image/jpeg");

    std::map<std::string, std::string> replacements;
    std::string urlTemplate = url_;
    if (!useKvp_) {
        replacements.insert({
            {"Layer", options_.layer},
            {"Style", options_.style},
            {"TileMatrix", matrix},
            {"TileRow", std::to_string(row)},
            {"TileCol", std::to_string(col)},
            {"TileMatrixSet", options_.tileMatrixSetId}});
        addSubdomain(replacements, options_, col, row, level);
        addDimensions(replacements, options_);
    } else {
        replacements.insert({
            {"layer", options_.layer},
            {"style", options_.style},
            {"tilematrix", matrix},
            {"tilerow", std::to_string(row)},
            {"tilecol", std::to_string(col)},
            {"tilematrixset", options_.tileMatrixSetId},
            {"format", format}});
        addDimensions(replacements, options_);
        addSubdomain(replacements, options_, col, row, level);

        const std::string separator =
            urlTemplate.find('?') == std::string::npos ? "?" : "&";
        urlTemplate +=
            separator +
            "request=GetTile&version=1.0.0&service=WMTS&"
            "format={format}&layer={layer}&style={style}&"
            "tilematrixset={tilematrixset}&"
            "tilematrix={tilematrix}&tilerow={tilerow}&tilecol={tilecol}";
    }

    return substituteTemplateParameters(urlTemplate, replacements);
}

} // namespace earth_engine
