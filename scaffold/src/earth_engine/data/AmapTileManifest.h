#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace earth_engine {

/// 高德 get_tile manifest 客户端(参考 xinzhi-map amap_manifest.js)。
/// 两步加载:
///   1) POST jsapi.amap.com/web_map/get_tile?key=<key>
///      body: version/pbf_version/access_oversea/data_source/multi_lang/
///            tiles=[{"id":"x_y_z","type":1|2,"contain_range":2},...]
///      → { infocode:"10000", tile_urls:[ ".../v2/<group>/pbf/<n>/x_y_z?auth_key=…" ] }
///   2) GET 签名 tile_urls → 容器(4 字节大端长度 + gzip(protobuf)),
///      交给 decodeAmapTile / decodeAmapPoiTile。
/// 鉴权 = key 查询参数 + Referer 白名单(无每请求签名;签名在返回 URL 上)。
/// 数据版本:静态默认会腐烂(旧 stamp 返回 83 字节空瓦),用前必须
/// resolveTileVersion 探测(参考同款,成功才缓存)。
struct AmapManifestConfig {
    std::string key;  // 高德 web key(域白名单)
    std::string version;  // 空 = 运行时探测
    std::string pbfVersion = "v2";
    std::string apiBase = "https://jsapi.amap.com/web_map/get_tile";
    std::string initBase = "https://jsapi.amap.com/web/init";
    std::string referer = "https://www.amap.com/";
    std::string accessOversea = "1";
    std::string dataSource = "1";
    std::string multiLang = "0";
};

struct AmapTileRequest {
    int x = 0;
    int y = 0;
    int z = 0;
    /// 图层组:1 = building_region_road_transit(线/面/建筑/轨道),
    /// 2 = poi_region_road_transit(POI 标签)。
    int type = 1;
};

struct AmapTileUrl {
    std::string group;
    std::string id;  // "<x>_<y>_<z>"
    std::string url;
};

/// 可注入同步 fetch(host 测试用假实现;真机接 CurlMultiRequestScheduler
/// get/post)。返回 false = 传输失败;status 填 HTTP 码。
using AmapHttpFetch =
    std::function<bool(const std::string& url, const std::string& method,
                       const std::string& body,
                       const std::vector<std::pair<std::string, std::string>>&
                           headers,
                       int& status, std::string& outBody)>;

/// 高德数据 zoom 档位(canonical z + 1 的 tier 表,参考同款):
/// 2-5→3, 6-7→6, 8-9→8, 10-12→10, 13-14→12, ≥15→14。
int amapDataZoom(int canonicalZ);

std::string buildGetTileUrl(const AmapManifestConfig& cfg);
std::string buildGetTileBody(const std::vector<AmapTileRequest>& requests,
                             const AmapManifestConfig& cfg,
                             const std::string& version);

/// 解析 get_tile JSON。infocode != "10000" → 失败(error 带 info);
/// 无 tile_urls = 该地面无数据(合法,非错误)。
bool parseTileUrls(const std::string& json, std::vector<AmapTileUrl>& out,
                   std::string* error = nullptr);

/// 版本探测:GET initBase?key= → {tile:"{\"v\":\"yy_mm_dd_hh\",…}"}
/// (双重解码)。失败返回 false;cfg.version 非空时直接返回其值。
bool resolveTileVersion(const AmapManifestConfig& cfg,
                        const AmapHttpFetch& fetch, std::string& version,
                        std::string* error = nullptr);

/// 批量 manifest:POST get_tile → 签名瓦片 URL(带 Referer 头)。
bool fetchAmapTileUrls(const std::vector<AmapTileRequest>& requests,
                       const AmapManifestConfig& cfg,
                       const AmapHttpFetch& fetch,
                       std::vector<AmapTileUrl>& out,
                       std::string* error = nullptr);

}  // namespace earth_engine
