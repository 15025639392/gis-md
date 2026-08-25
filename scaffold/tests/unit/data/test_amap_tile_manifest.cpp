#include <gtest/gtest.h>

#include "earth_engine/data/AmapTileManifest.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace earth_engine;

namespace {

AmapHttpFetch makeFakeFetch(
    std::map<std::string, std::string> responses,
    std::vector<std::pair<std::string, std::string>>* seen = nullptr) {
    return [responses = std::move(responses), seen](
               const std::string& url, const std::string& method,
               const std::string& body,
               const std::vector<std::pair<std::string, std::string>>& headers,
               int& status, std::string& outBody) {
        if (seen) seen->emplace_back(method, url);
        const auto it = responses.find(url);
        if (it == responses.end()) return false;
        outBody = it->second;
        status = 200;
        return true;
    };
}

}  // namespace

TEST(AmapTileManifestTest, DataZoomTierTable) {
    EXPECT_EQ(3, amapDataZoom(0));
    EXPECT_EQ(3, amapDataZoom(4));
    EXPECT_EQ(6, amapDataZoom(5));
    EXPECT_EQ(6, amapDataZoom(6));
    EXPECT_EQ(8, amapDataZoom(8));
    EXPECT_EQ(10, amapDataZoom(10));
    EXPECT_EQ(10, amapDataZoom(11));
    EXPECT_EQ(12, amapDataZoom(12));
    EXPECT_EQ(12, amapDataZoom(13));
    EXPECT_EQ(14, amapDataZoom(14));
    EXPECT_EQ(14, amapDataZoom(15));
}

TEST(AmapTileManifestTest, BuildGetTileBody) {
    AmapManifestConfig cfg;
    cfg.key = "abc";
    std::vector<AmapTileRequest> reqs = {{13038, 6785, 14, 1},
                                         {13038, 6785, 14, 2}};
    const std::string body = buildGetTileBody(reqs, cfg, "26_07_27_00");
    EXPECT_NE(std::string::npos, body.find("version=26_07_27_00"));
    EXPECT_NE(std::string::npos, body.find("pbf_version=v2"));
    EXPECT_NE(std::string::npos, body.find("13038_6785_14"));
    EXPECT_NE(std::string::npos, body.find("contain_range%22%3A2"));
}

TEST(AmapTileManifestTest, ParseTileUrls) {
    const std::string json =
        "{\"infocode\":\"10000\",\"tile_urls\":["
        "\"https://jsapi-data2.amap.com/tile/26_07_27_00/v2/"
        "building_region_road_transit/pbf/2/13038_6785_14?auth_key=x\","
        "\"https://jsapi-data3.amap.com/tile/26_07_27_00/v2/"
        "poi_region_road_transit/pbf/2/13038_6785_14?auth_key=y\"]}";
    std::vector<AmapTileUrl> urls;
    std::string err;
    ASSERT_TRUE(parseTileUrls(json, urls, &err)) << err;
    ASSERT_EQ(2u, urls.size());
    EXPECT_EQ("building_region_road_transit", urls[0].group);
    EXPECT_EQ("13038_6785_14", urls[0].id);
    EXPECT_EQ("poi_region_road_transit", urls[1].group);
    EXPECT_EQ("13038_6785_14", urls[1].id);
}

TEST(AmapTileManifestTest, ParseTileUrlsRefusal) {
    const std::string json =
        "{\"infocode\":\"10006\",\"info\":\"INVALID_USER_DOMAIN\"}";
    std::vector<AmapTileUrl> urls;
    std::string err;
    EXPECT_FALSE(parseTileUrls(json, urls, &err));
    EXPECT_NE(std::string::npos, err.find("10006"));
}

TEST(AmapTileManifestTest, ParseTileUrlsNoDataIsOk) {
    const std::string json = "{\"infocode\":\"10000\"}";
    std::vector<AmapTileUrl> urls;
    ASSERT_TRUE(parseTileUrls(json, urls));
    EXPECT_TRUE(urls.empty());
}

TEST(AmapTileManifestTest, SelectsRequestedGroupAndIdNotFirstUrl) {
    const std::vector<AmapTileUrl> urls = {
        {"building_region_road_transit", "13038_6784_14", "neighbor"},
        {"poi_region_road_transit", "13038_6785_14", "poi"},
        {"building_region_road_transit", "13038_6785_14", "requested"},
    };
    AmapTileUrl selected;
    std::string err;
    ASSERT_TRUE(selectAmapTileUrl(urls, {13038, 6785, 14, 1}, selected, &err))
        << err;
    EXPECT_EQ("requested", selected.url);
}

TEST(AmapTileManifestTest, RejectsMissingRequestedGroupOrId) {
    const std::vector<AmapTileUrl> urls = {
        {"building_region_road_transit", "13038_6784_14", "neighbor"},
        {"poi_region_road_transit", "13038_6785_14", "poi"},
    };
    AmapTileUrl selected;
    std::string err;
    EXPECT_FALSE(
        selectAmapTileUrl(urls, {13038, 6785, 14, 1}, selected, &err));
    EXPECT_NE(std::string::npos, err.find("13038_6785_14"));
}

TEST(AmapTileManifestTest, ResolveTileVersionDoubleDecodes) {
    AmapManifestConfig cfg;
    cfg.key = "k";
    auto fetch = makeFakeFetch(
        {{"https://jsapi.amap.com/web/init?key=k",
          "{\"tile\":\"{\\\"v\\\":\\\"26_07_27_00\\\",\\\"t\\\":\\\"0\\\"}\"}"}});
    std::string version;
    std::string err;
    ASSERT_TRUE(resolveTileVersion(cfg, fetch, version, &err)) << err;
    EXPECT_EQ("26_07_27_00", version);
}

TEST(AmapTileManifestTest, ResolveTileVersionRejectsMalformed) {
    AmapManifestConfig cfg;
    cfg.key = "k";
    auto fetch = makeFakeFetch(
        {{"https://jsapi.amap.com/web/init?key=k",
          "{\"tile\":\"{\\\"v\\\":\\\"oops\\\"}\"}"}});
    std::string version;
    std::string err;
    EXPECT_FALSE(resolveTileVersion(cfg, fetch, version, &err));
}

TEST(AmapTileManifestTest, FetchAmapTileUrlsPostsWithReferer) {
    AmapManifestConfig cfg;
    cfg.key = "k";
    cfg.referer = "https://www.amap.com/";
    std::vector<std::pair<std::string, std::string>> seen;
    std::vector<std::pair<std::string, std::string>> seenHeaders;
    auto fetch = [&](const std::string& url, const std::string& method,
                     const std::string& body,
                     const std::vector<std::pair<std::string, std::string>>&
                         headers,
                     int& status, std::string& outBody) {
        seen.emplace_back(method, url);
        for (const auto& h : headers) seenHeaders.push_back(h);
        if (method == "GET") {
            outBody =
                "{\"tile\":\"{\\\"v\\\":\\\"26_07_27_00\\\"}\"}";
        } else {
            outBody =
                "{\"infocode\":\"10000\",\"tile_urls\":["
                "\"https://jsapi-data2.amap.com/tile/26_07_27_00/v2/"
                "building_region_road_transit/pbf/2/13038_6785_14?auth_key=x\"]}";
        }
        status = 200;
        return true;
    };
    std::vector<AmapTileUrl> urls;
    std::string err;
    ASSERT_TRUE(fetchAmapTileUrls({{13038, 6785, 14, 1}}, cfg, fetch, urls,
                                  &err))
        << err;
    ASSERT_EQ(2u, seen.size());
    EXPECT_EQ("GET", seen[0].first);
    EXPECT_EQ("POST", seen[1].first);
    bool hasReferer = false;
    for (const auto& h : seenHeaders) {
        if (h.first == "Referer" && h.second == "https://www.amap.com/") {
            hasReferer = true;
        }
    }
    EXPECT_TRUE(hasReferer);
    ASSERT_EQ(1u, urls.size());
    EXPECT_EQ("building_region_road_transit", urls[0].group);
}
