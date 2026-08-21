#include <gtest/gtest.h>

#include "earth_engine/core/cache/HttpCache.h"

#include <memory>
#include <vector>

using namespace earth_engine;

// P1-7:HttpCache 共享存储与字节预算语义。

namespace {

std::vector<uint8_t> bodyOf(size_t n, uint8_t fill) {
    return std::vector<uint8_t>(n, fill);
}

} // namespace

TEST(HttpCacheTest, GetResponseSharesStorageWithoutCopy) {
    HttpCache cache(10);
    cache.put("a", bodyOf(16, 1));

    auto first = cache.getResponse("a");
    auto second = cache.getResponse("a");
    ASSERT_TRUE(first && second);
    // 命中返回同一共享条目,不再逐次深拷贝 body。
    EXPECT_EQ(first.get(), second.get());
    EXPECT_EQ(16u, first->body.size());
}

TEST(HttpCacheTest, SharedPutIsZeroCopyAndVisible) {
    HttpCache cache(10);
    CachedResponse resp;
    resp.body = bodyOf(8, 7);
    auto shared = std::make_shared<const CachedResponse>(std::move(resp));
    cache.putResponse("k", shared);

    auto got = cache.getResponse("k");
    ASSERT_TRUE(got);
    EXPECT_EQ(shared.get(), got.get());
}

TEST(HttpCacheTest, EvictsByEntryCountLru) {
    HttpCache cache(2);
    cache.put("a", bodyOf(4, 1));
    cache.put("b", bodyOf(4, 2));
    cache.get("a");  // a 变 MRU
    cache.put("c", bodyOf(4, 3));

    EXPECT_TRUE(cache.contains("a"));
    EXPECT_FALSE(cache.contains("b"));
    EXPECT_TRUE(cache.contains("c"));
    EXPECT_EQ(2u, cache.size());
}

TEST(HttpCacheTest, EvictsByByteBudget) {
    HttpCache cache(/*maxEntries=*/100, /*maxBodyBytes=*/100);
    cache.put("a", bodyOf(60, 1));
    cache.put("b", bodyOf(60, 2));  // 120 > 100 → 逐出 LRU 的 a

    EXPECT_FALSE(cache.contains("a"));
    EXPECT_TRUE(cache.contains("b"));
    EXPECT_EQ(60u, cache.bodyBytes());
}

TEST(HttpCacheTest, ReplaceAndRemoveKeepByteAccounting) {
    HttpCache cache(10, 1000);
    cache.put("a", bodyOf(100, 1));
    EXPECT_EQ(100u, cache.bodyBytes());
    cache.put("a", bodyOf(40, 2));  // 替换同 key
    EXPECT_EQ(40u, cache.bodyBytes());
    EXPECT_EQ(1u, cache.size());

    cache.remove("a");
    EXPECT_EQ(0u, cache.bodyBytes());
    EXPECT_EQ(0u, cache.size());
}

TEST(HttpCacheTest, ExpiredEntryDropsBytesOnAccess) {
    HttpCache cache(10, 1000);
    CachedResponse resp;
    resp.body = bodyOf(50, 3);
    resp.expiryTime = 1;  // 远古时刻,恒过期
    cache.putResponse("old", resp);
    EXPECT_EQ(50u, cache.bodyBytes());

    EXPECT_EQ(nullptr, cache.getResponse("old"));
    EXPECT_EQ(0u, cache.bodyBytes());
    EXPECT_EQ(0u, cache.size());
}

// ---- I-P1:响应头过期计算 / stale 重验语义 / 304 刷新 ----

TEST(HttpCacheTest, ComputeExpiryPrefersCacheControlMaxAge) {
    const std::time_t now = 1'700'000'000;
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Cache-Control", "public, max-age=600"},
        {"Expires", "Thu, 01 Jan 1970 00:00:01 GMT"},  // 被 max-age 忽略
    };
    EXPECT_EQ(now + 600, HttpCache::computeExpiryTime(200, headers, now));
}

TEST(HttpCacheTest, ComputeExpiryFallsBackToExpires) {
    const std::time_t now = 1'700'000'000;
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Expires", "Thu, 21 Nov 2023 08:49:37 GMT"},
    };
    // 2023-11-21 08:49:37 UTC 的 epoch。
    const std::time_t expected = 1700556577;
    EXPECT_EQ(expected, HttpCache::computeExpiryTime(200, headers, now));
}

TEST(HttpCacheTest, ComputeExpiryUsesValidatorWhenNoFreshnessLifetime) {
    const std::time_t now = 1'700'000'000;
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"ETag", "\"abc123\""},
    };
    // 无 max-age/Expires 但有验证器 → now(立即 stale,每次条件重验)。
    EXPECT_EQ(now, HttpCache::computeExpiryTime(200, headers, now));
}

TEST(HttpCacheTest, ComputeExpiryNoStoreAndBadStatusAreNotCacheable) {
    const std::time_t now = 1'700'000'000;
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Cache-Control", "no-store"},
    };
    EXPECT_EQ(0, HttpCache::computeExpiryTime(200, headers, now));
    EXPECT_EQ(0, HttpCache::computeExpiryTime(404, {{"ETag", "x"}}, now));
}

TEST(HttpCacheTest, RevalidationHeaderPrefersEtag) {
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Last-Modified", "Wed, 21 Oct 2015 07:28:00 GMT"},
        {"etag", "\"v2\""},  // 大小写不敏感
    };
    const auto reval = HttpCache::revalidationHeader(headers);
    ASSERT_TRUE(reval.has_value());
    EXPECT_EQ("If-None-Match", reval->first);
    EXPECT_EQ("\"v2\"", reval->second);
    EXPECT_TRUE(HttpCache::hasRevalidationHeader(headers));
}

TEST(HttpCacheTest, StaleEntryKeptForRevalidationThenRefreshed) {
    HttpCache cache(10, 1000);
    CachedResponse resp;
    resp.body = bodyOf(64, 9);
    resp.headers = {{"ETag", "\"v1\""}, {"Cache-Control", "max-age=0"}};
    resp.expiryTime = 1;  // 远古,恒过期
    cache.putResponse("tile", resp);

    // stale=true 时条目保留,供调用方拿 ETag 发条件请求。
    bool stale = false;
    auto staleEntry = cache.getResponse("tile", &stale);
    ASSERT_TRUE(staleEntry);
    EXPECT_TRUE(stale);
    EXPECT_EQ(64u, cache.bodyBytes());  // 未被删除
    EXPECT_EQ("If-None-Match",
              HttpCache::revalidationHeader(staleEntry->headers)->first);

    // 304 重验成功 → 只刷新过期时刻,body 保留(零拷贝)。
    cache.refreshExpiry("tile", std::time(nullptr) + 3600);
    stale = true;
    auto fresh = cache.getResponse("tile", &stale);
    ASSERT_TRUE(fresh);
    EXPECT_FALSE(stale);
    EXPECT_EQ(fresh.get(), staleEntry.get());  // 同一共享条目,未重建
    EXPECT_EQ(64u, fresh->body.size());
}

TEST(HttpCacheTest, StaleEntryWithoutStaleFlagDropsBytes) {
    HttpCache cache(10, 1000);
    CachedResponse resp;
    resp.body = bodyOf(32, 4);
    resp.expiryTime = 1;
    cache.putResponse("tile", resp);

    // 不传 stale 输出 = 旧语义:过期即删。
    EXPECT_EQ(nullptr, cache.getResponse("tile"));
    EXPECT_EQ(0u, cache.bodyBytes());
}

TEST(HttpCacheTest, ComputeExpiryHandlesSMaxAge) {
    const std::time_t now = 1'700'000'000;
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Cache-Control", "s-maxage=300"},
    };
    EXPECT_EQ(now + 300, HttpCache::computeExpiryTime(200, headers, now));
}
