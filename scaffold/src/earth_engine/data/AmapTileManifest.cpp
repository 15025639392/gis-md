#include "AmapTileManifest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace earth_engine {
namespace {

std::string urlEncode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            const char hex[] = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xf]);
        }
    }
    return out;
}

}  // namespace

int amapDataZoom(int canonicalZ) {
    const int amapZ = canonicalZ + 1;
    if (amapZ >= 15) return 14;
    if (amapZ >= 13) return 12;
    if (amapZ >= 10) return 10;
    if (amapZ >= 8) return 8;
    if (amapZ >= 6) return 6;
    return 3;
}

std::string buildGetTileUrl(const AmapManifestConfig& cfg) {
    return cfg.apiBase + "?key=" + urlEncode(cfg.key);
}

std::string buildGetTileBody(const std::vector<AmapTileRequest>& requests,
                             const AmapManifestConfig& cfg,
                             const std::string& version) {
    nlohmann::json tiles = nlohmann::json::array();
    for (const auto& r : requests) {
        nlohmann::json j;
        j["id"] = std::to_string(r.x) + "_" + std::to_string(r.y) + "_" +
                  std::to_string(r.z);
        j["type"] = r.type;
        j["contain_range"] = 2;
        tiles.push_back(std::move(j));
    }
    nlohmann::json params;
    params["version"] = version.empty() ? cfg.version : version;
    params["pbf_version"] = cfg.pbfVersion;
    params["access_oversea"] = cfg.accessOversea;
    params["data_source"] = cfg.dataSource;
    params["multi_lang"] = cfg.multiLang;
    params["tiles"] = tiles.dump();
    std::string body;
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (!body.empty()) body += "&";
        body += urlEncode(it.key()) + "=" + urlEncode(it.value().get<std::string>());
    }
    return body;
}

bool parseTileUrls(const std::string& json, std::vector<AmapTileUrl>& out,
                   std::string* error) {
    out.clear();
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (const std::exception& e) {
        if (error) *error = std::string("amap: manifest JSON parse failed: ") + e.what();
        return false;
    }
    const std::string infocode = doc.value("infocode", "");
    if (infocode != "10000") {
        if (error) {
            *error = "amap: get_tile refused, infocode=" + infocode +
                     " info=" + doc.value("info", "");
        }
        return false;
    }
    if (!doc.contains("tile_urls")) return true;  // 无数据地面
    for (const auto& u : doc["tile_urls"]) {
        const std::string url = u.get<std::string>();
        // 形如 .../v2/<group>/pbf/<n>/<x>_<y>_<z>?auth_key=…
        const std::string marker = "/pbf/";
        const size_t p = url.find(marker);
        if (p == std::string::npos) continue;
        const size_t gs = url.rfind("/v2/", p);
        const size_t ge = p;
        size_t idStart = p + marker.size();
        while (idStart < url.size() && std::isdigit(url[idStart])) ++idStart;
        if (idStart < url.size() && url[idStart] == '/') ++idStart;  // <n>/
        const size_t idBegin = idStart;
        while (idStart < url.size() && std::isdigit(url[idStart])) ++idStart;
        if (idStart < url.size() && url[idStart] == '_') {
            const size_t idEnd = url.find_first_of("?&", idStart);
            AmapTileUrl entry;
            entry.group = url.substr(gs + 4, ge - gs - 4);
            entry.id = url.substr(idBegin, idEnd - idBegin);
            entry.url = url;
            out.push_back(std::move(entry));
        }
    }
    return true;
}

bool selectAmapTileUrl(const std::vector<AmapTileUrl>& urls,
                       const AmapTileRequest& request, AmapTileUrl& out,
                       std::string* error) {
    const std::string expectedId = std::to_string(request.x) + "_" +
                                   std::to_string(request.y) + "_" +
                                   std::to_string(request.z);
    std::string expectedGroup;
    if (request.type == 1) {
        expectedGroup = "building_region_road_transit";
    } else if (request.type == 2) {
        expectedGroup = "poi_region_road_transit";
    } else {
        if (error) *error = "amap: unsupported tile request type=" +
                            std::to_string(request.type);
        return false;
    }
    const auto it = std::find_if(
        urls.begin(), urls.end(), [&](const AmapTileUrl& candidate) {
            return candidate.id == expectedId &&
                   candidate.group == expectedGroup && !candidate.url.empty();
        });
    if (it == urls.end()) {
        if (error) {
            *error = "amap: manifest has no matching URL for " + expectedGroup +
                     "/" + expectedId;
        }
        return false;
    }
    out = *it;
    return true;
}

bool resolveTileVersion(const AmapManifestConfig& cfg,
                        const AmapHttpFetch& fetch, std::string& version,
                        std::string* error) {
    if (!cfg.version.empty()) {
        version = cfg.version;
        return true;
    }
    const std::string url = cfg.initBase + "?key=" + urlEncode(cfg.key);
    int status = 0;
    std::string body;
    if (!fetch(url, "GET", "", {}, status, body)) {
        if (error) *error = "amap: version probe transport failed";
        return false;
    }
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(body);
        const std::string tile = doc.value("tile", "");
        const nlohmann::json inner = nlohmann::json::parse(tile);
        version = inner.value("v", "");
    } catch (const std::exception& e) {
        if (error) *error = std::string("amap: version probe parse failed: ") + e.what();
        return false;
    }
    if (version.size() != 11 || version[2] != '_' || version[5] != '_' ||
        version[8] != '_') {
        if (error) *error = "amap: malformed version stamp: " + version;
        return false;
    }
    return true;
}

bool fetchAmapTileUrls(const std::vector<AmapTileRequest>& requests,
                       const AmapManifestConfig& cfg,
                       const AmapHttpFetch& fetch,
                       std::vector<AmapTileUrl>& out, std::string* error) {
    out.clear();
    if (requests.empty()) return true;
    std::string version;
    if (!resolveTileVersion(cfg, fetch, version, error)) return false;
    const std::string url = buildGetTileUrl(cfg);
    const std::string body = buildGetTileBody(requests, cfg, version);
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/x-www-form-urlencoded"}};
    if (!cfg.referer.empty()) headers.emplace_back("Referer", cfg.referer);
    int status = 0;
    std::string resp;
    if (!fetch(url, "POST", body, headers, status, resp)) {
        if (error) *error = "amap: get_tile transport failed";
        return false;
    }
    return parseTileUrls(resp, out, error);
}

}  // namespace earth_engine
