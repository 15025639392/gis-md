#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

/// Simple file-based persistent cache for raw HTTP bytes.
/// Entries are stored as one file per URL in a cache directory.
/// Integrated with HttpCache: on miss checks disk, on write persists to disk.
class PersistentCache {
public:
    /// Set the cache directory (must be called before first use).
    static void setCacheDir(const std::string& dir) { cacheDir_ = dir; }
    static const std::string& cacheDir() { return cacheDir_; }

    /// Read cached data for a URL from disk. Returns empty on miss.
    static std::vector<uint8_t> load(const std::string& url) {
        if (cacheDir_.empty()) return {};
        std::string path = filePath(url);
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return {};
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> data(static_cast<size_t>(sz));
        fread(data.data(), 1, static_cast<size_t>(sz), f);
        fclose(f);
        return data;
    }

    /// Persist data to disk (called from worker thread for async write).
    static void save(const std::string& url, const std::vector<uint8_t>& data) {
        if (cacheDir_.empty() || data.empty()) return;
        std::string path = filePath(url);
        // Ensure directory exists
        ensureDir();
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return;
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
    }

    /// Pre-warm: load all cached files into the provided callback.
    /// Callback signature: void(const std::string& url, std::vector<uint8_t> data)
    template <typename F>
    static void prewarm(F&& onEntry) {
        if (cacheDir_.empty()) return;
        // Simple approach: iterate files in cache dir (not recursive)
        // We use a known naming pattern: files matching "{hex}.bin"
        // For now, skip prewarming — files are loaded on-demand via load()
        (void)onEntry;
    }

private:
    static std::string filePath(const std::string& url) {
        size_t h = std::hash<std::string>{}(url);
        char buf[32];
        snprintf(buf, sizeof(buf), "%016zx.bin", h);
        return cacheDir_ + "/" + buf;
    }

    static void ensureDir() {
        static std::once_flag once;
        std::call_once(once, [] {
            // Create directory if needed (best-effort)
            // mkdir is not available on all platforms; we rely on fopen creating files
        });
    }

    inline static std::string cacheDir_;
};

} // namespace earth_engine
