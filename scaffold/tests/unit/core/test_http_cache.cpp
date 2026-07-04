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
