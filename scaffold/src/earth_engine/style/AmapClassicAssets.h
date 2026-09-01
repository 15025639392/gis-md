#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../core/async/WorkLedger.h"

namespace earth_engine {

class Engine;
class HttpRequest;
class PlatformBridge;
class AmapClassicRuntime;
class SceneFrameResourceArbiter;

/// Atomic runtime asset lifecycle for the sealed AMap classic profile.
/// The host supplies only credentials; manifest
/// discovery, icon downloads, frame extraction, retries, and readiness are
/// owned here so an application cannot accidentally create a second visual
/// contract with generic Engine icon/font injection.
class AmapClassicAssets {
public:
    struct Credentials {
        std::string webKey;
    };

    ~AmapClassicAssets();

    AmapClassicAssets(const AmapClassicAssets&) = delete;
    AmapClassicAssets& operator=(const AmapClassicAssets&) = delete;

    void start();
    /// Render-thread pump: releases completed request handles, installs
    /// decoded official frames, and schedules bounded retries.
    void update(SceneFrameResourceArbiter& resourceArbiter);
    void reset();

    bool glyphsReady() const;
    bool iconsReady() const;
#if defined(EARTH_ENGINE_TESTING)
    void requireAtlasForContractTest(int atlas) { requireAtlas(atlas); }
    bool installAtlasForContractTest(int atlas, std::vector<uint8_t> body) {
        return installAtlas({atlas, std::move(body)});
    }
#endif

private:
    friend class AmapClassicRuntime;
    AmapClassicAssets(Engine& engine, PlatformBridge& platformBridge,
                      Credentials credentials);
    struct CallbackGate {
        std::mutex mutex;
        bool alive = true;
    };
    struct Download {
        int atlas = 0;
        std::vector<uint8_t> body;
    };
    struct GlyphDownload {
        std::vector<uint32_t> codepoints;
        std::vector<uint8_t> body;
    };

    static std::chrono::seconds retryDelay(int attempt);
    bool requestOneAtlas(SceneFrameResourceArbiter& resourceArbiter);
    bool requestOneGlyphBatch(SceneFrameResourceArbiter& resourceArbiter);
    bool installAtlas(Download download);
    void requireAtlas(int atlas);
    void requireGlyph(uint32_t codepoint);
    void installManifest(std::string version, std::string path,
                         std::string type);

    Engine& engine_;
    PlatformBridge& platformBridge_;
    Credentials credentials_;
    mutable std::mutex mutex_;
    uint64_t nextId_ = 0;
    std::unordered_map<uint64_t, std::unique_ptr<HttpRequest>> requests_;
    std::vector<uint64_t> completed_;
    std::vector<Download> landed_;
    std::vector<GlyphDownload> landedGlyphs_;
    std::unordered_set<int> installed_;
    std::unordered_set<int> requiredAtlases_;
    std::unordered_set<int> inFlightAtlases_;
    std::unordered_map<int, int> attempts_;
    std::unordered_map<int, std::chrono::steady_clock::time_point> nextRetry_;
    bool started_ = false;
    std::unordered_set<uint32_t> requiredGlyphs_;
    std::unordered_set<uint32_t> inFlightGlyphs_;
    std::unordered_set<uint32_t> installedGlyphs_;
    std::unordered_map<uint32_t, int> glyphAttempts_;
    std::string iconVersion_;
    std::string iconPath_;
    std::string iconType_;
    uint64_t generation_ = 0;
    std::optional<WorkLedger::Ticket> demandTicket_;
    std::shared_ptr<CallbackGate> callbackGate_ =
        std::make_shared<CallbackGate>();
};

} // namespace earth_engine
