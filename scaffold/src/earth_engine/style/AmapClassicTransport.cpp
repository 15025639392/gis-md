#include "AmapClassicRuntime.h"

#include "../data/AmapTileManifestInternal.h"
#include "../platform/bridge/PlatformBridge.h"

#include <algorithm>
#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {

struct AmapClassicRuntime::Transport::Impl {
    struct CallbackGate {
        std::recursive_mutex mutex;
        bool alive = true;
    };

    PlatformBridge& bridge;
    Credentials credentials;
    Transport::ManifestCallback manifestCallback;
    std::mutex mutex;
    std::string version;
    bool versionProbing = false;
    std::vector<std::function<void(bool, std::string)>> versionWaiters;
    uint64_t nextId = 0;
    std::unordered_map<uint64_t, std::unique_ptr<HttpRequest>> requests;
    std::vector<uint64_t> completed;
    std::shared_ptr<CallbackGate> callbackGate =
        std::make_shared<CallbackGate>();

    Impl(PlatformBridge& platformBridge, Credentials value,
         Transport::ManifestCallback callback)
        : bridge(platformBridge), credentials(std::move(value)),
          manifestCallback(std::move(callback)) {}

    uint64_t allocateId() {
        std::lock_guard<std::mutex> lock(mutex);
        return nextId++;
    }

    void hold(uint64_t id, std::unique_ptr<HttpRequest> request) {
        std::lock_guard<std::mutex> lock(mutex);
        if (std::find(completed.begin(), completed.end(), id) ==
            completed.end()) {
            requests[id] = std::move(request);
        }
    }

    void complete(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex);
        completed.push_back(id);
    }

    void fetchSignedUrl(
        std::string url, AmapClassicSourceBundle::FetchCallback callback) {
        const uint64_t id = allocateId();
        const auto gate = callbackGate;
        auto request = bridge.get(
            url,
            [this, gate, id, callback = std::move(callback)](
                int status, std::vector<uint8_t> body) mutable {
                std::lock_guard<std::recursive_mutex> gateLock(gate->mutex);
                if (!gate->alive) return;
                callback(status, std::move(body));
                complete(id);
            },
            HttpRequestOptions(HttpRequestPriority::Low,
                               {{"Referer", kAmapOfficialReferer}}));
        hold(id, std::move(request));
    }

    void fetchManifest(
        const TileKey& key, int requestType, std::string resolvedVersion,
        AmapClassicSourceBundle::FetchCallback callback) {
        AmapManifestConfig config;
        config.key = credentials.webKey;
        if (!credentials.apiBase.empty()) config.apiBase = credentials.apiBase;
        config.version = std::move(resolvedVersion);
        const std::string url = buildGetTileUrl(config);
        const std::string body = buildGetTileBody(
            {{key.x, key.y, key.z, requestType}}, config, config.version);
        const uint64_t id = allocateId();
        const auto gate = callbackGate;
        auto request = bridge.post(
            url, std::vector<uint8_t>(body.begin(), body.end()),
            "application/x-www-form-urlencoded",
            [this, gate, id, key, requestType,
             callback = std::move(callback)](
                int status, std::vector<uint8_t> bytes) mutable {
                std::lock_guard<std::recursive_mutex> gateLock(gate->mutex);
                if (!gate->alive) return;
                std::vector<AmapTileUrl> urls;
                std::string error;
                if (status != 200 ||
                    !parseTileUrls(
                        std::string(bytes.begin(), bytes.end()), urls,
                        &error)) {
                    callback(status, {});
                    complete(id);
                    return;
                }
                AmapTileUrl selected;
                if (!selectAmapTileUrl(
                        urls, {key.x, key.y, key.z, requestType}, selected,
                        &error)) {
                    // A successful manifest with no matching tile URL is a
                    // legal empty AMap tile, not a transport failure. Encode
                    // that result as a one-byte empty payload so the shared
                    // decoded cache can retain the distinction from a failed
                    // request and surface masks can publish transparent pages
                    // without entering permanent-failure backoff.
                    callback(200, {0});
                    complete(id);
                    return;
                }
                fetchSignedUrl(selected.url, std::move(callback));
                complete(id);
            },
            HttpRequestOptions(HttpRequestPriority::Low,
                               {{"Referer", kAmapOfficialReferer}}));
        hold(id, std::move(request));
    }

    void fetch(const TileKey& key, int requestType,
               AmapClassicSourceBundle::FetchCallback callback) {
        if (credentials.webKey.empty()) {
            callback(401, {});
            return;
        }
        bool startProbe = false;
        std::string resolvedVersion;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!version.empty()) {
                resolvedVersion = version;
            } else {
                versionWaiters.push_back(
                    [this, key, requestType,
                     callback = std::move(callback)](
                        bool ok, std::string value) mutable {
                        if (!ok) {
                            callback(503, {});
                            return;
                        }
                        fetchManifest(key, requestType, std::move(value),
                                      std::move(callback));
                    });
                if (!versionProbing) {
                    versionProbing = true;
                    startProbe = true;
                }
            }
        }
        if (!resolvedVersion.empty()) {
            fetchManifest(key, requestType, std::move(resolvedVersion),
                          std::move(callback));
            return;
        }
        if (!startProbe) return;

        AmapManifestConfig config;
        config.key = credentials.webKey;
        if (!credentials.initBase.empty()) config.initBase = credentials.initBase;
        const std::string url = config.initBase + "?key=" + credentials.webKey;
        const uint64_t id = allocateId();
        const auto gate = callbackGate;
        auto request = bridge.get(
            url,
            [this, gate, id](int status, std::vector<uint8_t> bytes) {
                std::lock_guard<std::recursive_mutex> gateLock(gate->mutex);
                if (!gate->alive) return;
                std::string resolved;
                std::string iconVersion, iconPath, iconType;
                if (status == 200) {
                    try {
                        const auto outer =
                            nlohmann::json::parse(bytes.begin(), bytes.end());
                        const auto inner = nlohmann::json::parse(
                            outer.value("tile", "{}"));
                        resolved = inner.value("v", "");
                        const auto icon = nlohmann::json::parse(
                            outer.value("icon", "{}"));
                        iconVersion = icon.value("v", "");
                        iconPath = icon.value("p", "");
                        iconType = icon.value("t", "");
                    } catch (const std::exception&) {
                        resolved.clear();
                    }
                }
                std::vector<std::function<void(bool, std::string)>> waiters;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (resolved.size() == 11 && resolved[2] == '_' &&
                        resolved[5] == '_' && resolved[8] == '_') {
                        version = resolved;
                    } else {
                        resolved.clear();
                    }
                    versionProbing = false;
                    waiters.swap(versionWaiters);
                }
                for (auto& waiter : waiters) {
                    waiter(!resolved.empty(), resolved);
                }
                if (!iconVersion.empty() && !iconPath.empty() &&
                    !iconType.empty() && manifestCallback) {
                    manifestCallback(std::move(iconVersion),
                                     std::move(iconPath),
                                     std::move(iconType));
                }
                complete(id);
            },
            HttpRequestOptions(HttpRequestPriority::Low,
                               {{"Referer", kAmapOfficialReferer}}));
        hold(id, std::move(request));
    }
};

AmapClassicRuntime::Transport::Transport(
    PlatformBridge& platformBridge, Credentials credentials,
    ManifestCallback manifestCallback)
    : impl_(std::make_unique<Impl>(platformBridge, std::move(credentials),
                                  std::move(manifestCallback))) {}

AmapClassicRuntime::Transport::~Transport() {
    {
        std::lock_guard<std::recursive_mutex> gateLock(
            impl_->callbackGate->mutex);
        impl_->callbackGate->alive = false;
    }
    std::unordered_map<uint64_t, std::unique_ptr<HttpRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        requests.swap(impl_->requests);
        impl_->completed.clear();
        impl_->versionWaiters.clear();
    }
    requests.clear();
}

void AmapClassicRuntime::Transport::fetchType1(
    const TileKey& key, AmapClassicSourceBundle::FetchCallback callback) {
    impl_->fetch(key, 1, std::move(callback));
}

void AmapClassicRuntime::Transport::fetchPoi(
    const TileKey& key, AmapClassicSourceBundle::FetchCallback callback) {
    impl_->fetch(key, 2, std::move(callback));
}

void AmapClassicRuntime::Transport::update() {
    std::unordered_map<uint64_t, std::unique_ptr<HttpRequest>> completed;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (uint64_t id : impl_->completed) {
            auto it = impl_->requests.find(id);
            if (it == impl_->requests.end()) continue;
            completed.emplace(id, std::move(it->second));
            impl_->requests.erase(it);
        }
        impl_->completed.clear();
    }
    completed.clear();
}

} // namespace earth_engine
