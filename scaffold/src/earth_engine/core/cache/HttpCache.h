#pragma once

#include "PersistentCache.h"
#include "../async/AsyncSystem.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {

/// A cached HTTP response with status, headers, body, and expiry.
struct CachedResponse {
    int statusCode = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<uint8_t> body;
    std::time_t expiryTime = 0; // 0 = no expiry

    bool isExpired() const {
        return expiryTime > 0 && std::time(nullptr) >= expiryTime;
    }
};

namespace detail {

inline std::string trimAsciiWhitespace(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

inline bool asciiCaseInsensitiveEqual(const std::string& a,
                                      const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char ca = static_cast<char>(
            std::tolower(static_cast<unsigned char>(a[i])));
        const char cb = static_cast<char>(
            std::tolower(static_cast<unsigned char>(b[i])));
        if (ca != cb) return false;
    }
    return true;
}

/// 大小写不敏感地在响应头里找名字,返回指向值的迭代器(找不到=end)。
inline std::vector<std::pair<std::string, std::string>>::const_iterator
findHeaderCaseInsensitive(
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& name) {
    return std::find_if(
        headers.begin(),
        headers.end(),
        [&name](const std::pair<std::string, std::string>& h) {
            return asciiCaseInsensitiveEqual(h.first, name);
        });
}

/// RFC 7231 IMF-fixdate 转 UTC epoch(纯整数算法,无平台 timegm 依赖)。
/// 解析失败返回 -1。
inline std::time_t parseHttpDateToEpoch(const std::string& httpDate) {
    static const char* kMonths[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    // "Sun, 06 Nov 1994 08:49:37 GMT" → skip 周几 + 逗号空格。
    size_t pos = httpDate.find(',');
    if (pos == std::string::npos || pos + 2 > httpDate.size()) return -1;
    pos += 2;  // ", "
    auto readNumber = [&httpDate, &pos](size_t maxDigits) -> long {
        long v = 0;
        size_t digits = 0;
        while (pos < httpDate.size() && digits < maxDigits &&
               httpDate[pos] >= '0' && httpDate[pos] <= '9') {
            v = v * 10 + (httpDate[pos] - '0');
            ++pos;
            ++digits;
        }
        return v;
    };
    const long day = readNumber(2);
    if (pos >= httpDate.size() || httpDate[pos] != ' ') return -1;
    ++pos;
    int month = -1;
    for (int i = 0; i < 12; ++i) {
        const size_t len = 3;
        if (pos + len <= httpDate.size() &&
            httpDate.compare(pos, len, kMonths[i]) == 0) {
            month = i;
            break;
        }
    }
    if (month < 0) return -1;
    pos += 3;
    if (pos >= httpDate.size() || httpDate[pos] != ' ') return -1;
    ++pos;
    const long year = readNumber(4);
    if (pos >= httpDate.size() || httpDate[pos] != ' ') return -1;
    ++pos;
    const long hour = readNumber(2);
    if (pos >= httpDate.size() || httpDate[pos] != ':') return -1;
    ++pos;
    const long minute = readNumber(2);
    if (pos >= httpDate.size() || httpDate[pos] != ':') return -1;
    ++pos;
    const long second = readNumber(2);
    if (day < 1 || day > 31 || year < 1970 || hour > 23 || minute > 59 ||
        second > 60) {
        return -1;
    }
    // days-from-civil(Howard Hinnant 算法):自 1970-01-01 的整天数。
    const long long y = static_cast<long long>(year) - (month < 2 ? 1 : 0);
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const long long yoe = y - era * 400;
    const long long doy = (153 * (month + (month > 2 ? -2 : 10)) + 2) / 5 +
                          static_cast<long long>(day) - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097 + doe - 719468;
    return static_cast<std::time_t>(days * 86400LL + hour * 3600 +
                                    minute * 60 + second);
}

}  // namespace detail

/// Thread-safe LRU cache for HTTP responses with expiry and pruning.
/// Enhanced to match cesium-native CachingAssetAccessor capabilities.
///
/// P1-7:条目存 shared_ptr<const CachedResponse>——命中直接返回共享引用
/// (零拷贝、锁内零分配),put 全链 move,持久化任务捕获同一 shared_ptr。
/// 容量双限:条数 + body 字节预算(旧实现 2000 条×75KB ≈ 150MB 无上限)。
class HttpCache {
public:
    static constexpr size_t kDefaultMaxBodyBytes = 128ull * 1024 * 1024;

    explicit HttpCache(size_t maxEntries = 2000,
                       size_t maxBodyBytes = kDefaultMaxBodyBytes)
        : maxEntries_(maxEntries), maxBodyBytes_(maxBodyBytes) {}

    // --- Backward-compat body-only API (returns raw bytes) ---

    /// @deprecated Use getResponse() to share the cached body without a copy.
    std::vector<uint8_t> get(const std::string& url) {
        auto resp = getResponse(url);
        return resp ? resp->body : std::vector<uint8_t>{};
    }

    /// @deprecated Use put(url, std::move(data)) or putResponse().
    void put(const std::string& url, const std::vector<uint8_t>& data) {
        CachedResponse resp;
        resp.body = data;
        putResponse(url,
                    std::make_shared<const CachedResponse>(std::move(resp)));
    }

    void put(const std::string& url, std::vector<uint8_t>&& data) {
        CachedResponse resp;
        resp.body = std::move(data);
        putResponse(url,
                    std::make_shared<const CachedResponse>(std::move(resp)));
    }

    // --- Full response API ---

    /// Get cached response. Returns nullptr if not cached or expired.
    /// The returned pointer shares storage with the cache entry — no copy.
    /// @param stale 可选输出:非空时,过期条目不删除而是置 *stale=true 并返回
    ///        (供调用方拿 ETag/Last-Modified 发条件请求重验);新鲜命中置
    ///        *stale=false。为空时行为与旧版一致(过期即删,返回 nullptr)。
    std::shared_ptr<const CachedResponse> getResponse(const std::string& url,
                                                       bool* stale = nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++lookups_;
        auto it = map_.find(url);
        if (it == map_.end()) return nullptr;
        if (entryExpired(it->second)) {
            if (stale) {
                *stale = true;
                touch(url);
                return it->second.response;
            }
            eraseLocked(it);
            return nullptr;
        }
        if (stale) *stale = false;
        touch(url);
        ++hits_;
        return it->second.response;
    }

    /// I-P1:304 Not-Modified 后刷新条目过期时间,保留 body/headers 零拷贝
    /// (只改 O(1) 的 override,不重建 CachedResponse)。条目不存在时静默忽略。
    void refreshExpiry(const std::string& url, std::time_t newExpiry) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(url);
        if (it == map_.end()) return;
        it->second.expiryOverride = newExpiry;
        touch(url);
    }

    /// I-P1:从响应头计算缓存过期时刻(对齐 cesium CachingAssetAccessor):
    ///  - Cache-Control:max-age / s-maxage 优先(秒)
    ///  - 否则 Expires(HTTP-date)
    ///  - 都没有但有 ETag/Last-Modified → now(立即 stale,每次条件重验)
    ///  - 全无 → 0(无缓存语义,调用方据此决定不缓存)
    /// @param statusCode 非 2xx 且非 304 返回 0(不可缓存);304 用于重验后
    ///        按 304 响应头计算新过期时刻(cesium updateCacheItem 同语义)。
    static std::time_t computeExpiryTime(
        int statusCode,
        const std::vector<std::pair<std::string, std::string>>& headers,
        std::time_t now = std::time(nullptr)) {
        if (statusCode != 304 && (statusCode < 200 || statusCode >= 300)) {
            return 0;
        }
        const auto cacheControlIt = detail::findHeaderCaseInsensitive(
            headers, "Cache-Control");
        if (cacheControlIt != headers.end()) {
            // 逐条解析 "max-age=600, public, s-maxage=3600" 这类值。
            std::string value = cacheControlIt->second;
            size_t start = 0;
            while (start <= value.size()) {
                size_t comma = value.find(',', start);
                const std::string directive = detail::trimAsciiWhitespace(
                    value.substr(start, comma == std::string::npos
                                             ? std::string::npos
                                             : comma - start));
                const size_t eq = directive.find('=');
                const std::string name =
                    eq == std::string::npos
                        ? directive
                        : detail::trimAsciiWhitespace(directive.substr(0, eq));
                const std::string arg =
                    eq == std::string::npos
                        ? ""
                        : detail::trimAsciiWhitespace(directive.substr(eq + 1));
                if (detail::asciiCaseInsensitiveEqual(name, "no-store")) {
                    return 0;
                }
                if (detail::asciiCaseInsensitiveEqual(name, "max-age") ||
                    detail::asciiCaseInsensitiveEqual(name, "s-maxage")) {
                    char* end = nullptr;
                    const long age = std::strtol(arg.c_str(), &end, 10);
                    if (end != arg.c_str() && age >= 0) {
                        return now + static_cast<std::time_t>(age);
                    }
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        const auto expiresIt =
            detail::findHeaderCaseInsensitive(headers, "Expires");
        if (expiresIt != headers.end()) {
            const std::time_t parsed =
                detail::parseHttpDateToEpoch(expiresIt->second);
            if (parsed > 0) return parsed;
        }
        const auto etagIt = detail::findHeaderCaseInsensitive(headers, "ETag");
        const auto lastModifiedIt =
            detail::findHeaderCaseInsensitive(headers, "Last-Modified");
        if (etagIt != headers.end() || lastModifiedIt != headers.end()) {
            return now;  // 有验证器:缓存但立即 stale → 每次条件重验
        }
        return 0;
    }

    /// I-P1:从缓存条目提取条件请求头(If-None-Match 优先,否则
    /// If-Modified-Since),供过期条目重验;无验证器返回 nullopt。
    static std::optional<std::pair<std::string, std::string>>
    revalidationHeader(
        const std::vector<std::pair<std::string, std::string>>& headers) {
        const auto etagIt = detail::findHeaderCaseInsensitive(headers, "ETag");
        if (etagIt != headers.end()) {
            return std::make_pair(std::string("If-None-Match"),
                                  etagIt->second);
        }
        const auto lastModifiedIt =
            detail::findHeaderCaseInsensitive(headers, "Last-Modified");
        if (lastModifiedIt != headers.end()) {
            return std::make_pair(std::string("If-Modified-Since"),
                                  lastModifiedIt->second);
        }
        return std::nullopt;
    }

    /// I-P1:该缓存条目是否携带可重验的验证器(ETag / Last-Modified)。
    static bool hasRevalidationHeader(
        const std::vector<std::pair<std::string, std::string>>& headers) {
        return revalidationHeader(headers).has_value();
    }

    /// Store response in cache (copies once into shared storage).
    void putResponse(const std::string& url, const CachedResponse& response) {
        putResponse(url, std::make_shared<const CachedResponse>(response));
    }

    /// Store an already-shared response — zero copy; the persistence task
    /// captures the same shared_ptr.
    void putResponse(const std::string& url,
                     std::shared_ptr<const CachedResponse> response) {
        if (!response) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = map_.find(url);
            if (it != map_.end()) {
                currentBodyBytes_ -= it->second.response->body.size();
                currentBodyBytes_ += response->body.size();
                it->second.response = response;
                it->second.expiryOverride = 0;
                touch(url);
            } else {
                lru_.push_front(url);
                lruMap_[url] = lru_.begin();
                currentBodyBytes_ += response->body.size();
                map_[url] = Entry{response, 0};
            }
            while ((map_.size() > maxEntries_ ||
                    currentBodyBytes_ > maxBodyBytes_) &&
                   !lru_.empty()) {
                evictOne();
            }
        }
        persistAsync(url, std::move(response));
    }

    /// Remove a single entry.
    void remove(const std::string& url) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(url);
        if (it != map_.end()) {
            eraseLocked(it);
        }
    }

    /// Remove expired entries.
    size_t prune() {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        auto it = map_.begin();
        while (it != map_.end()) {
            if (entryExpired(it->second)) {
                currentBodyBytes_ -= it->second.response->body.size();
                auto lruIt = lruMap_.find(it->first);
                if (lruIt != lruMap_.end()) {
                    lru_.erase(lruIt->second);
                    lruMap_.erase(lruIt);
                }
                it = map_.erase(it);
                ++count;
            } else {
                ++it;
            }
        }
        return count;
    }

    /// Clear all cached entries.
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        map_.clear();
        lru_.clear();
        lruMap_.clear();
        currentBodyBytes_ = 0;
    }

    /// Check if URL is cached and not expired.
    bool contains(const std::string& url) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(url);
        if (it == map_.end()) return false;
        if (entryExpired(it->second)) {
            eraseLocked(it);
            return false;
        }
        return true;
    }

    /// Number of entries.
    size_t size() const { return map_.size(); }

    /// Total cached body bytes (approximate, headers excluded).
    size_t bodyBytes() const { return currentBodyBytes_; }

    /// 累计查表次数与命中次数(进程生命期内单调递增,不随 clear/prune 归零)。
    ///
    /// 为什么要:跨运行比较加载耗时时,**缓存温度不同会直接把结论比反**——冷
    /// 启动与热缓存的同一段相机轨迹不是同一件工作。这两个数让"这次是冷还是
    /// 热"成为可读事实,而不是靠回忆上一次跑过什么。
    ///
    /// 只计 getResponse(get() 亦经由它);contains() 是存在性探询不是取用,不计。
    /// 与 HttpCache 的只读契约不冲突:这里不碰响应体,只数自己被问了几次。
    struct Stats {
        uint64_t lookups = 0;
        uint64_t hits = 0;
    };
    Stats stats() {
        std::lock_guard<std::mutex> lock(mutex_);
        return Stats{lookups_, hits_};
    }

    /// Singleton for global use.
    static HttpCache& shared() {
        static HttpCache instance(2000);
        return instance;
    }

    void setMaxEntries(size_t n) { maxEntries_ = n; }
    size_t maxEntries() const { return maxEntries_; }

private:
    struct Entry {
        std::shared_ptr<const CachedResponse> response;
        /// I-P1:304 重验后的过期覆盖(0 = 用 response->expiryTime)。
        /// 独立字段而非重建 CachedResponse:body 可能几百 KB,304 只改
        /// 过期时刻,重建 = 网络线程上无谓的整条 body 深拷贝。
        std::time_t expiryOverride = 0;
    };

    void touch(const std::string& url) {
        auto lruIt = lruMap_.find(url);
        if (lruIt != lruMap_.end()) {
            lru_.erase(lruIt->second);
            lru_.push_front(url);
            lruMap_[url] = lru_.begin();
        }
    }

    /// 条目过期判定:304 重验的 override 优先(见 Entry::expiryOverride)。
    static bool entryExpired(const Entry& entry) {
        const std::time_t expiry = entry.expiryOverride != 0
            ? entry.expiryOverride
            : entry.response->expiryTime;
        return expiry > 0 && std::time(nullptr) >= expiry;
    }

    // LRU 位置一律经 lruMap_ 查(touch 后它是唯一保持同步的索引)。
    void eraseLocked(std::unordered_map<std::string, Entry>::iterator it) {
        currentBodyBytes_ -= it->second.response->body.size();
        auto lruIt = lruMap_.find(it->first);
        if (lruIt != lruMap_.end()) {
            lru_.erase(lruIt->second);
            lruMap_.erase(lruIt);
        }
        map_.erase(it);
    }

    void evictOne() {
        if (lru_.empty()) return;
        std::string victim = lru_.back();
        lru_.pop_back();
        lruMap_.erase(victim);
        auto it = map_.find(victim);
        if (it != map_.end()) {
            currentBodyBytes_ -= it->second.response->body.size();
            map_.erase(it);
        }
    }

    void persistAsync(const std::string& url,
                      std::shared_ptr<const CachedResponse> response) {
        if (!PersistentCache::cacheDir().empty() && !response->body.empty()) {
            AsyncSystem::pool().enqueue(
                [u = url, response = std::move(response)] {
                    PersistentCache::save(u, response->body);
                });
        }
    }

    size_t maxEntries_;
    size_t maxBodyBytes_;
    size_t currentBodyBytes_ = 0;
    // 命中率统计,全程在 mutex_ 保护下读写(与 map_ 同一把锁,无额外同步成本)。
    uint64_t lookups_ = 0;
    uint64_t hits_ = 0;
    std::mutex mutex_;
    std::list<std::string> lru_;
    std::unordered_map<std::string, std::list<std::string>::iterator> lruMap_;
    std::unordered_map<std::string, Entry> map_;
};

} // namespace earth_engine
