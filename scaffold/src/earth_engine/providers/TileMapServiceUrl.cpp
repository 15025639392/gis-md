#include "TileMapServiceUrl.h"

#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Projection.h"
#include "earth_engine/tiling/TileScheme.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace earth_engine {
namespace {

bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(),
                         suffix.size(),
                         suffix) == 0;
}

std::string normalizePath(const std::string& path) {
    const bool absolute = !path.empty() && path.front() == '/';
    std::vector<std::string> segments;

    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find('/', start);
        const std::string segment = path.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        if (segment == "..") {
            if (!segments.empty() && segments.back() != "..") {
                segments.pop_back();
            } else if (!absolute) {
                segments.push_back(segment);
            }
        } else if (!segment.empty() && segment != ".") {
            segments.push_back(segment);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    std::string normalized = absolute ? "/" : "";
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) normalized.push_back('/');
        normalized += segments[i];
    }
    return normalized.empty() && absolute ? "/" : normalized;
}

std::string resolveRelativeUrl(const std::string& baseUrl,
                               const std::string& relativeUrl) {
    if (relativeUrl.find("://") != std::string::npos ||
        relativeUrl.rfind("//", 0) == 0) {
        return relativeUrl;
    }

    const size_t baseSuffixStart = baseUrl.find_first_of("?#");
    const std::string basePrefix = baseSuffixStart == std::string::npos
        ? baseUrl
        : baseUrl.substr(0, baseSuffixStart);

    std::string relativePath = relativeUrl;
    std::string relativeSuffix;
    const size_t relativeSuffixStart = relativePath.find_first_of("?#");
    if (relativeSuffixStart != std::string::npos) {
        relativeSuffix = relativePath.substr(relativeSuffixStart);
        relativePath.erase(relativeSuffixStart);
    }

    std::string prefix;
    std::string basePath;
    const size_t schemePos = basePrefix.find("://");
    if (schemePos != std::string::npos) {
        const size_t pathStart = basePrefix.find('/', schemePos + 3);
        if (pathStart == std::string::npos) {
            prefix = basePrefix;
            basePath = "/";
        } else {
            prefix = basePrefix.substr(0, pathStart);
            basePath = basePrefix.substr(pathStart);
        }
    } else {
        basePath = basePrefix;
    }

    std::string mergedPath;
    if (!relativePath.empty() && relativePath.front() == '/') {
        mergedPath = relativePath;
    } else {
        const size_t slash = basePath.rfind('/');
        const std::string directory = slash == std::string::npos
            ? std::string()
            : basePath.substr(0, slash + 1);
        mergedPath = directory + relativePath;
    }

    return prefix + normalizePath(mergedPath) + relativeSuffix;
}

bool isXmlNameCharacter(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) || value == '_' ||
           value == '-' || value == ':' || value == '.';
}

void skipWhitespace(std::string_view value, size_t& index) {
    while (index < value.size() &&
           std::isspace(static_cast<unsigned char>(value[index]))) {
        ++index;
    }
}

std::optional<std::string> attributeValue(std::string_view tag,
                                          std::string_view name) {
    size_t index = 0;
    while (index < tag.size()) {
        skipWhitespace(tag, index);
        if (index >= tag.size() || tag[index] == '<' || tag[index] == '/' ||
            tag[index] == '>') {
            ++index;
            continue;
        }

        const size_t nameStart = index;
        while (index < tag.size() && isXmlNameCharacter(tag[index])) {
            ++index;
        }
        const std::string_view attributeName =
            tag.substr(nameStart, index - nameStart);

        skipWhitespace(tag, index);
        if (index >= tag.size() || tag[index] != '=') {
            continue;
        }
        ++index;
        skipWhitespace(tag, index);
        if (index >= tag.size()) {
            return std::nullopt;
        }

        std::string value;
        if (tag[index] == '"' || tag[index] == '\'') {
            const char quote = tag[index++];
            const size_t valueStart = index;
            const size_t valueEnd = tag.find(quote, valueStart);
            if (valueEnd == std::string_view::npos) {
                return std::nullopt;
            }
            value = std::string(tag.substr(valueStart, valueEnd - valueStart));
            index = valueEnd + 1;
        } else {
            const size_t valueStart = index;
            while (index < tag.size() &&
                   !std::isspace(static_cast<unsigned char>(tag[index])) &&
                   tag[index] != '/' && tag[index] != '>') {
                ++index;
            }
            value = std::string(tag.substr(valueStart, index - valueStart));
        }

        if (attributeName == name) {
            return value;
        }
    }

    return std::nullopt;
}

std::optional<uint32_t> attributeUint32(std::string_view tag,
                                        std::string_view name) {
    const std::optional<std::string> value = attributeValue(tag, name);
    if (!value) {
        return std::nullopt;
    }

    size_t consumed = 0;
    const unsigned long parsed = std::stoul(*value, &consumed, 10);
    if (consumed != value->size() ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        throw std::out_of_range("TMS uint32 attribute is out of range");
    }
    return static_cast<uint32_t>(parsed);
}

std::optional<double> attributeDouble(std::string_view tag,
                                      std::string_view name) {
    const std::optional<std::string> value = attributeValue(tag, name);
    if (!value) {
        return std::nullopt;
    }

    size_t consumed = 0;
    const double parsed = std::stod(*value, &consumed);
    if (consumed != value->size()) {
        throw std::invalid_argument("TMS double attribute has trailing data");
    }
    return parsed;
}

std::optional<std::string_view> rootContent(std::string_view xml) {
    size_t rootStart = xml.find('<');
    while (rootStart != std::string_view::npos) {
        if (rootStart + 1 >= xml.size()) {
            return std::nullopt;
        }
        const char marker = xml[rootStart + 1];
        if (marker != '?' && marker != '!') {
            break;
        }
        const std::string_view close = marker == '?' ? "?>" : ">";
        const size_t skipped = xml.find(close, rootStart + 2);
        if (skipped == std::string_view::npos) {
            return std::nullopt;
        }
        rootStart = xml.find('<', skipped + close.size());
    }
    if (rootStart == std::string_view::npos || rootStart + 1 >= xml.size() ||
        xml[rootStart + 1] == '/') {
        return std::nullopt;
    }

    size_t nameStart = rootStart + 1;
    while (nameStart < xml.size() &&
           std::isspace(static_cast<unsigned char>(xml[nameStart]))) {
        ++nameStart;
    }
    size_t index = nameStart;
    while (index < xml.size() && isXmlNameCharacter(xml[index])) {
        ++index;
    }
    if (index == nameStart) {
        return std::nullopt;
    }
    const std::string rootName(xml.substr(nameStart, index - nameStart));
    const size_t openEnd = xml.find('>', index);
    if (openEnd == std::string_view::npos || openEnd == rootStart ||
        xml[openEnd - 1] == '/') {
        return std::string_view();
    }

    const std::string close = "</" + rootName + ">";
    const size_t closeStart = xml.rfind(close);
    if (closeStart == std::string_view::npos || closeStart <= openEnd) {
        return std::nullopt;
    }
    return xml.substr(openEnd + 1, closeStart - openEnd - 1);
}

std::optional<std::string_view> directChildTag(std::string_view xml,
                                               std::string_view tagName) {
    size_t index = 0;
    int depth = 0;
    while (index < xml.size()) {
        const size_t start = xml.find('<', index);
        if (start == std::string_view::npos) {
            return std::nullopt;
        }
        if (start + 1 >= xml.size()) {
            return std::nullopt;
        }
        const char marker = xml[start + 1];
        if (marker == '?' || marker == '!') {
            const size_t skipped =
                xml.find(marker == '?' ? "?>" : ">", start + 2);
            if (skipped == std::string_view::npos) {
                return std::nullopt;
            }
            index = skipped + (marker == '?' ? 2 : 1);
            continue;
        }
        if (marker == '/') {
            depth = std::max(0, depth - 1);
            const size_t end = xml.find('>', start + 2);
            if (end == std::string_view::npos) {
                return std::nullopt;
            }
            index = end + 1;
            continue;
        }

        const size_t nameStart = start + 1;
        size_t nameEnd = nameStart;
        while (nameEnd < xml.size() && isXmlNameCharacter(xml[nameEnd])) {
            ++nameEnd;
        }
        const size_t end = xml.find('>', nameEnd);
        if (end == std::string_view::npos) {
            return std::nullopt;
        }
        if (depth == 0 &&
            xml.substr(nameStart, nameEnd - nameStart) == tagName) {
            return xml.substr(start, end - start + 1);
        }
        if (end == start || xml[end - 1] != '/') {
            ++depth;
        }
        index = end + 1;
    }
    return std::nullopt;
}

std::optional<std::string> directChildElementText(std::string_view xml,
                                                  std::string_view tagName) {
    const std::optional<std::string_view> tag = directChildTag(xml, tagName);
    if (!tag) {
        return std::nullopt;
    }

    const size_t start = tag->data() - xml.data() + tag->size();
    const std::string close = "</" + std::string(tagName) + ">";
    const size_t end = xml.find(close, start);
    if (end == std::string_view::npos) {
        return std::string();
    }
    return std::string(xml.substr(start, end - start));
}

std::optional<std::string_view> directChildElementContent(
    std::string_view xml,
    std::string_view tagName) {
    const std::optional<std::string_view> tag = directChildTag(xml, tagName);
    if (!tag) {
        return std::nullopt;
    }

    const size_t start = tag->data() - xml.data() + tag->size();
    const std::string close = "</" + std::string(tagName) + ">";
    const size_t end = xml.find(close, start);
    if (end == std::string_view::npos) {
        return std::string_view();
    }
    return xml.substr(start, end - start);
}

std::vector<std::string_view> directChildTags(std::string_view xml,
                                              std::string_view tagName) {
    std::vector<std::string_view> result;
    size_t index = 0;
    while (index < xml.size()) {
        const std::optional<std::string_view> tag =
            directChildTag(xml.substr(index), tagName);
        if (!tag) {
            break;
        }
        result.push_back(*tag);
        index = static_cast<size_t>(tag->data() - xml.data()) + tag->size();
    }
    return result;
}

void parseProfileAndSrs(std::string_view xml, TileMapServiceMetadata& metadata) {
    std::string profile = "mercator";
    if (const std::optional<std::string_view> tileSets =
            directChildTag(xml, "TileSets")) {
        profile = attributeValue(*tileSets, "profile").value_or("mercator");
    }

    if (profile == "mercator" || profile == "global-mercator") {
        metadata.schemeId = "TMS-WebMercator";
        metadata.boundingBoxCoordinatesInDegrees =
            profile.rfind("global-", 0) != 0;
        return;
    }

    if (profile == "geodetic" || profile == "global-geodetic") {
        metadata.schemeId = "Geographic-TMS";
        metadata.boundingBoxCoordinatesInDegrees = true;
        return;
    }

    const std::optional<std::string> srs = directChildElementText(xml, "SRS");
    if (!srs) {
        return;
    }

    if (srs->find("4326") != std::string::npos) {
        metadata.schemeId = "Geographic-TMS";
        metadata.boundingBoxCoordinatesInDegrees = true;
    } else if (srs->find("3857") != std::string::npos ||
               srs->find("900913") != std::string::npos) {
        metadata.schemeId = "TMS-WebMercator";
        metadata.boundingBoxCoordinatesInDegrees = true;
    }
}

Projection projectionForSchemeId(const std::string& schemeId) {
    if (schemeId == "Geographic-TMS") {
        return GeographicProjection(Ellipsoid::WGS84());
    }
    return WebMercatorProjection(Ellipsoid::WGS84());
}

void parseBoundingBox(std::string_view xml, TileMapServiceMetadata& metadata) {
    const std::optional<std::string_view> boundingBox =
        directChildTag(xml, "BoundingBox");
    if (!boundingBox) {
        return;
    }

    const std::optional<double> west = attributeDouble(*boundingBox, "minx");
    const std::optional<double> south = attributeDouble(*boundingBox, "miny");
    const std::optional<double> east = attributeDouble(*boundingBox, "maxx");
    const std::optional<double> north = attributeDouble(*boundingBox, "maxy");
    if (!west || !south || !east || !north) {
        return;
    }

    if (!metadata.boundingBoxCoordinatesInDegrees) {
        metadata.projectedCoverageRectangle =
            Rectangle(*west, *south, *east, *north);
        return;
    }

    metadata.projectedCoverageRectangle = projectRectangleSimple(
        projectionForSchemeId(metadata.schemeId),
        Rectangle::fromDegrees(*west, *south, *east, *north));
}

} // namespace

std::string tileMapServiceXmlUrl(const std::string& url) {
    constexpr const char* kResource = "tilemapresource.xml";

    const size_t suffixStart = url.find_first_of("?#");
    const std::string prefix =
        suffixStart == std::string::npos ? url : url.substr(0, suffixStart);
    const std::string suffix =
        suffixStart == std::string::npos ? std::string() : url.substr(suffixStart);

    if (endsWith(prefix, kResource)) {
        return url;
    }

    std::string path = prefix;
    if (endsWith(path, ".xml")) {
        const size_t slash = path.find_last_of('/');
        if (slash == std::string::npos) {
            path = kResource;
        } else {
            path.erase(slash + 1);
            path += kResource;
        }
        return path + suffix;
    }

    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path += kResource;
    return path + suffix;
}

std::string tileMapServiceTileUrl(const std::string& baseUrl,
                                  const std::string& tileSetUrl,
                                  int x,
                                  int y,
                                  const std::string& fileExtension) {
    const std::string relative = tileSetUrl + "/" + std::to_string(x) + "/" +
                                 std::to_string(y) + fileExtension;
    return resolveRelativeUrl(baseUrl, relative);
}

std::optional<std::string> tileMapServiceTileUrlForKey(
    const std::string& baseUrl,
    const TileMapServiceMetadata& metadata,
    const TileKey& key) {
    if (key.z < 0 ||
        static_cast<uint32_t>(key.z) < metadata.minimumLevel) {
        return std::nullopt;
    }

    const uint32_t tileSetIndex =
        static_cast<uint32_t>(key.z) - metadata.minimumLevel;
    if (tileSetIndex >= metadata.tileSets.size()) {
        return std::nullopt;
    }

    const std::string extension = metadata.fileExtension.empty()
        ? std::string()
        : "." + metadata.fileExtension;
    return tileMapServiceTileUrl(baseUrl,
                                 metadata.tileSets[tileSetIndex].url,
                                 key.x,
                                 key.y,
                                 extension);
}

std::optional<Rectangle> tileMapServiceGeographicCoverageRectangle(
    const TileMapServiceMetadata& metadata) {
    if (!metadata.projectedCoverageRectangle) {
        return std::nullopt;
    }

    return unprojectRectangleSimple(
        projectionForSchemeId(metadata.schemeId),
        *metadata.projectedCoverageRectangle);
}

Rectangle tileMapServiceResolvedGeographicCoverageRectangle(
    const TileMapServiceMetadata& metadata) {
    if (const std::optional<Rectangle> coverage =
            tileMapServiceGeographicCoverageRectangle(metadata)) {
        return *coverage;
    }

    if (metadata.schemeId == "Geographic-TMS") {
        return GeographicProjection::maximumGlobeRectangle();
    }
    return WebMercatorProjection::maximumGlobeRectangle();
}

std::unique_ptr<TileScheme> tileMapServiceTileScheme(
    const TileMapServiceMetadata& metadata) {
    if (metadata.schemeId == "Geographic-TMS") {
        return TileScheme::createGeographicTMS();
    }
    return TileScheme::createTMS();
}

bool tileMapServiceXmlIsLoadable(const std::string& xml) {
    const std::string_view view(xml);
    const std::optional<std::string_view> root = rootContent(view);
    if (!root) {
        return false;
    }

    if (!directChildTag(*root, "TileSets")) {
        return false;
    }

    const std::optional<std::string> srs =
        directChildElementText(*root, "SRS");
    if (!srs) {
        return false;
    }

    return srs->find("4326") != std::string::npos ||
           srs->find("3857") != std::string::npos ||
           srs->find("900913") != std::string::npos;
}

TileMapServiceMetadata parseTileMapServiceMetadata(const std::string& xml) {
    TileMapServiceMetadata metadata;
    const std::string_view view(xml);
    const std::string_view root = rootContent(view).value_or(view);

    parseProfileAndSrs(root, metadata);
    parseBoundingBox(root, metadata);

    if (const std::optional<std::string_view> tileFormat =
            directChildTag(root, "TileFormat")) {
        metadata.fileExtension =
            attributeValue(*tileFormat, "extension").value_or("png");
        metadata.tileWidth =
            attributeUint32(*tileFormat, "width").value_or(256);
        metadata.tileHeight =
            attributeUint32(*tileFormat, "height").value_or(256);
    }

    uint32_t minimumLevel = std::numeric_limits<uint32_t>::max();
    uint32_t maximumLevel = 0;

    const std::optional<std::string_view> tileSets =
        directChildElementContent(root, "TileSets");
    for (const std::string_view tag :
         tileSets ? directChildTags(*tileSets, "TileSet")
                  : std::vector<std::string_view>()) {
        TileMapServiceTileSet tileSet;
        tileSet.level = attributeUint32(tag, "order").value_or(0);
        tileSet.url =
            attributeValue(tag, "href").value_or(std::to_string(tileSet.level));
        metadata.tileSets.push_back(tileSet);

        minimumLevel = std::min(minimumLevel, tileSet.level);
        maximumLevel = std::max(maximumLevel, tileSet.level);
    }

    if (maximumLevel < minimumLevel && maximumLevel == 0) {
        minimumLevel = 0;
        maximumLevel = 25;
    }

    metadata.minimumLevel = std::min(minimumLevel, maximumLevel);
    metadata.maximumLevel = maximumLevel;
    return metadata;
}

} // namespace earth_engine
