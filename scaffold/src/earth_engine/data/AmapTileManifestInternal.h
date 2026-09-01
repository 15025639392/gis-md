#pragma once

#include <string>
#include <vector>

namespace earth_engine {

inline constexpr char kAmapOfficialReferer[] = "https://www.amap.com/";

// Runtime-private official protocol constants. Production callers cannot
// supply alternate endpoints, data sources, or protocol versions.
struct AmapManifestConfig {
    std::string key;
    std::string version;
    std::string pbfVersion = "v2";
    std::string apiBase = "https://jsapi.amap.com/web_map/get_tile";
    std::string initBase = "https://jsapi.amap.com/web/init";
    std::string referer = kAmapOfficialReferer;
    std::string accessOversea = "1";
    std::string dataSource = "1";
    std::string multiLang = "0";
};

struct AmapTileRequest {
    int x = 0;
    int y = 0;
    int z = 0;
    int type = 1;
};

struct AmapTileUrl {
    std::string group;
    std::string id;
    std::string url;
};

std::string buildGetTileUrl(const AmapManifestConfig& cfg);
std::string buildGetTileBody(const std::vector<AmapTileRequest>& requests,
                             const AmapManifestConfig& cfg,
                             const std::string& version);
bool parseTileUrls(const std::string& json, std::vector<AmapTileUrl>& out,
                   std::string* error = nullptr);
bool selectAmapTileUrl(const std::vector<AmapTileUrl>& urls,
                       const AmapTileRequest& request, AmapTileUrl& out,
                       std::string* error = nullptr);

}  // namespace earth_engine
