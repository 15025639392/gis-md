#include "TileMapServiceUrl.h"

#include <string>

namespace earth_engine {
namespace {

bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(),
                         suffix.size(),
                         suffix) == 0;
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

} // namespace earth_engine
