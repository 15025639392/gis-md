#include "AmapVectorConfig.h"

#include <nlohmann/json.hpp>

#include <array>
#include <string>
#include <vector>

namespace earth_engine::minimal_globe_demo {
namespace {

using nlohmann::json;

// Reject unknown keys inside a known section (fail-loud).
std::string rejectUnknown(const json& section, const char* sectionName,
                          const std::vector<std::string>& known) {
    for (auto it = section.begin(); it != section.end(); ++it) {
        bool ok = false;
        for (const auto& k : known) ok = ok || it.key() == k;
        if (!ok) {
            return std::string("unknown key '") + it.key() + "' in section '" +
                   sectionName + "'";
        }
    }
    return {};
}

std::string parseHexColor(const std::string& hex, std::array<float, 4>& out) {
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (hex.size() != 7 && hex.size() != 9) {
        return "color must be #RRGGBB or #AARRGGBB";
    }
    if (hex[0] != '#') return "color must start with '#'";
    const bool hasAlpha = hex.size() == 9;
    const int off = hasAlpha ? 2 : 0;
    std::array<int, 4> parts{};
    for (int i = 0; i < 3; ++i) {
        const int hi = hexVal(hex[off + 1 + i * 2]);
        const int lo = hexVal(hex[off + 2 + i * 2]);
        if (hi < 0 || lo < 0) return "color has non-hex digit";
        parts[i] = hi * 16 + lo;
    }
    if (hasAlpha) {
        const int hi = hexVal(hex[1]);
        const int lo = hexVal(hex[2]);
        if (hi < 0 || lo < 0) return "color has non-hex alpha";
        out = {static_cast<float>(parts[0]) / 255.0f,
               static_cast<float>(parts[1]) / 255.0f,
               static_cast<float>(parts[2]) / 255.0f,
               static_cast<float>(hi * 16 + lo) / 255.0f};
    } else {
        out = {static_cast<float>(parts[0]) / 255.0f,
               static_cast<float>(parts[1]) / 255.0f,
               static_cast<float>(parts[2]) / 255.0f, 1.0f};
    }
    return {};
}

}  // namespace

std::string parseAmapVectorConfig(const std::string& jsonText,
                                  AmapVectorConfig& out) {
    json root;
    try {
        root = json::parse(jsonText);
    } catch (const std::exception& e) {
        return std::string("invalid JSON: ") + e.what();
    }
    if (!root.is_object()) return "root must be a JSON object";

    auto err = rejectUnknown(root, "root", {"sources", "zooms", "style"});
    if (!err.empty()) return err;

    // ---- sources ----
    if (root.contains("sources")) {
        const json& src = root["sources"];
        err = rejectUnknown(src, "sources", {"amap", "terrain"});
        if (!err.empty()) return err;
        if (src.contains("amap")) {
            const json& amap = src["amap"];
            err = rejectUnknown(amap, "sources.amap",
                                {"apiBase", "initBase", "iconBase",
                                 "sdfBase"});
            if (!err.empty()) return err;
            if (amap.contains("apiBase")) out.apiBase = amap["apiBase"];
            if (amap.contains("initBase")) out.initBase = amap["initBase"];
            if (amap.contains("iconBase")) out.iconBase = amap["iconBase"];
            if (amap.contains("sdfBase")) out.sdfBase = amap["sdfBase"];
            out.hasAmapEndpoints = true;
        }
        if (src.contains("terrain")) {
            const json& t = src["terrain"];
            err = rejectUnknown(t, "sources.terrain",
                                {"urlTemplate", "minZoom", "maxZoom",
                                 "tileSize", "borderInset"});
            if (!err.empty()) return err;
            if (!t.contains("urlTemplate"))
                return "sources.terrain requires 'urlTemplate'";
            out.terrainUrlTemplate = t["urlTemplate"];
            if (t.contains("minZoom")) out.terrainMinZoom = t["minZoom"];
            if (t.contains("maxZoom")) out.terrainMaxZoom = t["maxZoom"];
            if (t.contains("tileSize")) out.terrainTileSize = t["tileSize"];
            if (t.contains("borderInset"))
                out.terrainBorderInset = t["borderInset"];
            out.hasTerrain = true;
        }
    }

    // ---- zooms ----
    if (root.contains("zooms")) {
        const json& z = root["zooms"];
        err = rejectUnknown(
            z, "zooms",
            {"minZoom", "regionsMaxZoom", "mainMaxZoom", "poiMaxZoom",
             "regionsActiveBelowZoom", "regionsSupportedZooms",
             "mainSupportedZooms", "poiSupportedZooms", "dataZoom"});
        if (!err.empty()) return err;
        if (z.contains("minZoom")) out.zoomMinZoom = z["minZoom"];
        if (z.contains("regionsMaxZoom"))
            out.zoomRegionsMaxZoom = z["regionsMaxZoom"];
        if (z.contains("mainMaxZoom"))
            out.zoomMainMaxZoom = z["mainMaxZoom"];
        if (z.contains("regionsActiveBelowZoom"))
            out.zoomRegionsActiveBelowZoom = z["regionsActiveBelowZoom"];
        if (z.contains("regionsSupportedZooms"))
            out.zoomRegionsSupported = z["regionsSupportedZooms"].get<
                std::vector<int>>();
        if (z.contains("mainSupportedZooms"))
            out.zoomMainSupported = z["mainSupportedZooms"].get<
                std::vector<int>>();
        if (z.contains("poiSupportedZooms"))
            out.zoomPoiSupported = z["poiSupportedZooms"].get<
                std::vector<int>>();
        if (z.contains("dataZoom")) {
            const json& dz = z["dataZoom"];
            if (!dz.is_array()) return "zooms.dataZoom must be an array";
            for (const auto& pair : dz) {
                if (!pair.is_array() || pair.size() != 2)
                    return "zooms.dataZoom entries must be [canonical, data]";
                out.zoomDataZoomRemap.emplace_back(pair[0].get<int>(),
                                                   pair[1].get<int>());
            }
        }
        out.hasZooms = true;
    }

    // ---- style ----
    if (root.contains("style")) {
        const json& st = root["style"];
        err = rejectUnknown(st, "style", {"surface", "line"});
        if (!err.empty()) return err;
        if (st.contains("surface")) {
            const json& surf = st["surface"];
            if (!surf.is_array()) return "style.surface must be an array";
            for (const auto& item : surf) {
                err = rejectUnknown(item, "style.surface[]",
                                    {"classCode", "subKey", "color"});
                if (!err.empty()) return err;
                if (!item.contains("classCode") || !item.contains("subKey") ||
                    !item.contains("color"))
                    return "style.surface[] requires classCode/subKey/color";
                AmapClassicStyleOverrides::Surface o;
                o.classCode = item["classCode"].get<int>();
                o.subKey = item["subKey"].get<int>();
                err = parseHexColor(item["color"].get<std::string>(), o.color);
                if (!err.empty()) return err;
                out.styleOverrides.surface.push_back(std::move(o));
            }
        }
        if (st.contains("line")) {
            const json& ln = st["line"];
            if (!ln.is_array()) return "style.line must be an array";
            for (const auto& item : ln) {
                err = rejectUnknown(item, "style.line[]",
                                    {"classCode", "subKey", "color",
                                     "widthPx"});
                if (!err.empty()) return err;
                if (!item.contains("classCode") || !item.contains("subKey"))
                    return "style.line[] requires classCode/subKey";
                AmapClassicStyleOverrides::Line o;
                o.classCode = item["classCode"].get<int>();
                o.subKey = item["subKey"].get<int>();
                if (item.contains("color")) {
                    err = parseHexColor(item["color"].get<std::string>(),
                                        o.color);
                    if (!err.empty()) return err;
                } else {
                    o.color = {0, 0, 0, 1};
                }
                o.widthPx = item.value("widthPx", 0.0f);
                out.styleOverrides.line.push_back(std::move(o));
            }
        }
        out.hasStyle = true;
    }
    return {};
}

} // namespace earth_engine::minimal_globe_demo