#include "WebMapServiceImageryProvider.h"

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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

bool isXmlNameCharacter(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) || value == '_' ||
           value == '-' || value == ':' || value == '.';
}

std::optional<std::string_view> firstElementRange(std::string_view xml,
                                                  std::string_view tagName,
                                                  size_t searchStart = 0) {
    const std::string openNeedle = "<" + std::string(tagName);
    size_t open = xml.find(openNeedle, searchStart);
    while (open != std::string_view::npos) {
        const size_t nameEnd = open + openNeedle.size();
        if (nameEnd >= xml.size() || isXmlNameCharacter(xml[nameEnd])) {
            open = xml.find(openNeedle, nameEnd);
            continue;
        }

        const size_t openEnd = xml.find('>', nameEnd);
        if (openEnd == std::string_view::npos) {
            return std::nullopt;
        }
        if (openEnd > open && xml[openEnd - 1] == '/') {
            return xml.substr(open, openEnd - open + 1);
        }

        const std::string closeNeedle = "</" + std::string(tagName) + ">";
        const size_t close = xml.find(closeNeedle, openEnd + 1);
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        return xml.substr(open, close + closeNeedle.size() - open);
    }
    return std::nullopt;
}

std::optional<std::string_view> elementContentRange(
    std::string_view element) {
    const size_t openEnd = element.find('>');
    if (openEnd == std::string_view::npos ||
        (openEnd > 0 && element[openEnd - 1] == '/')) {
        return std::nullopt;
    }

    const size_t closeStart = element.rfind("</");
    if (closeStart == std::string_view::npos || closeStart < openEnd + 1) {
        return std::nullopt;
    }
    return element.substr(openEnd + 1, closeStart - openEnd - 1);
}

std::optional<std::string_view> firstChildElementRange(
    std::string_view element,
    std::string_view tagName) {
    const std::optional<std::string_view> content =
        elementContentRange(element);
    if (!content) {
        return std::nullopt;
    }

    size_t searchStart = 0;
    while (searchStart < content->size()) {
        const size_t open = content->find('<', searchStart);
        if (open == std::string_view::npos) {
            break;
        }
        if (open + 1 >= content->size()) {
            return std::nullopt;
        }

        const char firstNameChar = (*content)[open + 1];
        if (firstNameChar == '/' || firstNameChar == '!' ||
            firstNameChar == '?') {
            const size_t tagEnd = content->find('>', open + 1);
            if (tagEnd == std::string_view::npos) {
                return std::nullopt;
            }
            searchStart = tagEnd + 1;
            continue;
        }

        size_t nameEnd = open + 1;
        while (nameEnd < content->size() &&
               isXmlNameCharacter((*content)[nameEnd])) {
            ++nameEnd;
        }
        if (nameEnd == open + 1) {
            return std::nullopt;
        }

        const std::string_view childName =
            content->substr(open + 1, nameEnd - open - 1);
        const std::optional<std::string_view> child =
            firstElementRange(*content, childName, open);
        if (!child) {
            return std::nullopt;
        }
        if (childName == tagName) {
            return child;
        }

        searchStart = open + child->size();
    }

    return std::nullopt;
}

std::optional<std::string_view> documentRootElementRange(std::string_view xml) {
    size_t searchStart = 0;
    while (searchStart < xml.size()) {
        const size_t open = xml.find('<', searchStart);
        if (open == std::string_view::npos || open + 1 >= xml.size()) {
            return std::nullopt;
        }

        const char firstNameChar = xml[open + 1];
        if (firstNameChar == '!' || firstNameChar == '?') {
            const size_t tagEnd = xml.find('>', open + 1);
            if (tagEnd == std::string_view::npos) {
                return std::nullopt;
            }
            searchStart = tagEnd + 1;
            continue;
        }

        size_t nameEnd = open + 1;
        while (nameEnd < xml.size() && isXmlNameCharacter(xml[nameEnd])) {
            ++nameEnd;
        }
        if (nameEnd == open + 1) {
            return std::nullopt;
        }

        return firstElementRange(xml, xml.substr(open + 1, nameEnd - open - 1),
                                 open);
    }

    return std::nullopt;
}

std::optional<std::string> firstChildElementText(std::string_view element,
                                                 std::string_view tagName) {
    const std::optional<std::string_view> child =
        firstChildElementRange(element, tagName);
    if (!child) {
        return std::nullopt;
    }
    const std::optional<std::string_view> content =
        elementContentRange(*child);
    if (!content) {
        return std::string();
    }
    return std::string(*content);
}

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

int countConfiguredWmsLayers(const std::string& layers) {
    int count = 0;
    std::stringstream sstream(layers);
    std::string layer;
    while (std::getline(sstream, layer, ',')) {
        if (!layer.empty()) {
            ++count;
        }
    }
    return count;
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

WebMapServiceCapabilitiesValidation validateWebMapServiceCapabilities(
    const std::string& capabilitiesXml,
    const WebMapServiceImageryOptions& options) {
    const std::optional<std::string_view> root =
        documentRootElementRange(capabilitiesXml);
    if (!root) {
        return WebMapServiceCapabilitiesValidation{
            false,
            "Web map service XML document does not have a root element."};
    }

    const std::optional<std::string_view> service =
        firstChildElementRange(*root, "Service");
    if (!service) {
        return WebMapServiceCapabilitiesValidation{
            false,
            "Web map service XML document does not have a Service element. "};
    }

    if (!firstChildElementRange(*service, "Name")) {
        return WebMapServiceCapabilitiesValidation{
            false,
            "Invalid web map service XML document (Service > Name is missing) "};
    }

    if (const std::optional<std::string> maxWidthText =
            firstChildElementText(*service, "MaxWidth")) {
        try {
            const int maxWidth = std::stoi(*maxWidthText);
            if (options.tileWidth > maxWidth) {
                return WebMapServiceCapabilitiesValidation{
                    false,
                    "configured tile width (" +
                        std::to_string(options.tileWidth) +
                        ") exceeds Service >> MaxWidth defined in WMS "
                        "document (" +
                        std::to_string(maxWidth) + ")."};
            }
        } catch (const std::invalid_argument&) {
            return WebMapServiceCapabilitiesValidation{
                false,
                "Invalid web map service XML document"};
        }
    }

    if (const std::optional<std::string> maxHeightText =
            firstChildElementText(*service, "MaxHeight")) {
        try {
            const int maxHeight = std::stoi(*maxHeightText);
            if (options.tileHeight > maxHeight) {
                return WebMapServiceCapabilitiesValidation{
                    false,
                    "configured tile height (" +
                        std::to_string(options.tileHeight) +
                        ") exceeds Service >> MaxHeight defined in WMS "
                        "document (" +
                        std::to_string(maxHeight) + ")."};
            }
        } catch (const std::invalid_argument&) {
            return WebMapServiceCapabilitiesValidation{
                false,
                "Invalid web map service XML document"};
        }
    }

    if (const std::optional<std::string> layerLimitText =
            firstChildElementText(*service, "LayerLimit")) {
        try {
            const int layerLimit = std::stoi(*layerLimitText);
            const int layerCount = countConfiguredWmsLayers(options.layers);
            if (layerCount > layerLimit) {
                return WebMapServiceCapabilitiesValidation{
                    false,
                    "the number of configured layers (" +
                        std::to_string(layerCount) +
                        ") exceeds WMS LayerLimit " +
                        std::to_string(layerLimit)};
            }
        } catch (const std::invalid_argument&) {
            return WebMapServiceCapabilitiesValidation{
                false,
                "Invalid web map service XML document"};
        }
    }

    return WebMapServiceCapabilitiesValidation{true, std::string()};
}

std::string webMapServiceCapabilitiesUrl(const std::string& baseUrl,
                                         const std::string& version) {
    UrlParts parts = splitUrl(baseUrl);
    setQueryValue(parts.query, "request", "GetCapabilities", true);
    setQueryValue(parts.query, "version", version, true);
    setQueryValue(parts.query, "service", "WMS", true);
    return joinUrl(parts);
}

} // namespace earth_engine
