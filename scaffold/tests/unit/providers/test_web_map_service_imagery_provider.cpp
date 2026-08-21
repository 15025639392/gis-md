#include <gtest/gtest.h>

#include "earth_engine/providers/WebMapServiceImageryProvider.h"
#include "earth_engine/tiling/TileKey.h"

#include <string>

using namespace earth_engine;

// I-V10:WMS version 适配。1.1.1 必须用 SRS 参数(lat,lon 轴序);1.3.0 用
// CRS 参数。此前 version 可配但只实现 1.3.0 口径 —— 配 1.1.1 时参数名
// crs 是错的(规范强制 SRS),请求会被服务拒绝或返回错误图层。

namespace {

std::string queryOf(const std::string& url) {
    const size_t q = url.find('?');
    return q == std::string::npos ? std::string() : url.substr(q + 1);
}

bool hasQueryPair(const std::string& query, const std::string& key,
                  const std::string& value) {
    const std::string needle = key + "=" + value;
    size_t pos = 0;
    while ((pos = query.find(key + "=", pos)) != std::string::npos) {
        const size_t amp = query.find('&', pos);
        const std::string pair = query.substr(
            pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (pair == needle) {
            return true;
        }
        pos += key.size();
    }
    return false;
}

}  // namespace

TEST(WebMapServiceImageryProviderTest, Version111UsesSrsParam) {
    WebMapServiceImageryOptions options;
    options.version = "1.1.1";
    options.layers = "test-layer";
    WebMapServiceImageryProvider provider(
        "https://wms.example.com/service", options);

    const std::string url = provider.buildUrl(TileKey{"Geographic-TMS", 1, 0, 0});
    const std::string query = queryOf(url);

    EXPECT_TRUE(hasQueryPair(query, "srs", "EPSG:4326")) << url;
    EXPECT_FALSE(hasQueryPair(query, "crs", "EPSG:4326")) << url;
    EXPECT_TRUE(hasQueryPair(query, "version", "1.1.1")) << url;
    // 1.1.1 轴序:lat,lon = south,west,north,east。
    EXPECT_TRUE(hasQueryPair(query, "bbox",
                             "-90.000000,-180.000000,0.000000,-90.000000")) << url;
}

TEST(WebMapServiceImageryProviderTest, Version130KeepsCrsParam) {
    WebMapServiceImageryOptions options;
    options.version = "1.3.0";
    options.layers = "test-layer";
    WebMapServiceImageryProvider provider(
        "https://wms.example.com/service", options);

    const std::string url = provider.buildUrl(TileKey{"Geographic-TMS", 1, 0, 0});
    const std::string query = queryOf(url);

    EXPECT_TRUE(hasQueryPair(query, "crs", "EPSG:4326")) << url;
    EXPECT_FALSE(hasQueryPair(query, "srs", "EPSG:4326")) << url;
    EXPECT_TRUE(hasQueryPair(query, "version", "1.3.0")) << url;
}

TEST(WebMapServiceImageryProviderTest, DefaultVersionIs130) {
    WebMapServiceImageryOptions options;
    options.layers = "default-layer";
    WebMapServiceImageryProvider provider(
        "https://wms.example.com/service", options);

    const std::string url = provider.buildUrl(TileKey{"Geographic-TMS", 2, 1, 1});
    const std::string query = queryOf(url);

    EXPECT_TRUE(hasQueryPair(query, "crs", "EPSG:4326")) << url;
    EXPECT_TRUE(hasQueryPair(query, "version", "1.3.0")) << url;
}

TEST(WebMapServiceImageryProviderTest, Version10UsesSrsLike111) {
    WebMapServiceImageryOptions options;
    options.version = "1.0.0";
    options.layers = "legacy";
    WebMapServiceImageryProvider provider(
        "https://wms.example.com/service", options);

    const std::string url = provider.buildUrl(TileKey{"Geographic-TMS", 1, 0, 0});
    const std::string query = queryOf(url);

    // 1.0.x 同 1.1.x:参数名 SRS。
    EXPECT_TRUE(hasQueryPair(query, "srs", "EPSG:4326")) << url;
    EXPECT_FALSE(hasQueryPair(query, "crs", "EPSG:4326")) << url;
}
