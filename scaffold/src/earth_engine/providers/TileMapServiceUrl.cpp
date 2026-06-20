#include "TileMapServiceUrl.h"

#include <string>
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

} // namespace earth_engine
