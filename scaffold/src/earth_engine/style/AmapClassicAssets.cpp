#include "AmapClassicAssets.h"

#include "AmapClassicLabelStyleInternal.h"
#include "../Engine.h"
#include "../data/AmapTileManifestInternal.h"
#include "../core/async/WorkLedger.h"
#include "../core/resources/SceneFrameResourceArbiter.h"
#include "../debug/PlatformLog.h"
#include "../platform/bridge/PlatformBridge.h"
#include "../renderer/GlyphAtlas.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace earth_engine {

namespace {
std::optional<std::vector<uint8_t>> decodePngDataUrl(
    const std::string& url) {
    constexpr char kPrefix[] = "data:image/png;base64,";
    if (url.rfind(kPrefix, 0) != 0) return std::nullopt;
    const std::string_view input(url.data() + sizeof(kPrefix) - 1,
                                 url.size() - sizeof(kPrefix) + 1);
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    if (input.empty() || input.size() % 4 != 0) return std::nullopt;
    std::vector<uint8_t> output;
    output.reserve(input.size() / 4 * 3);
    for (size_t i = 0; i < input.size(); i += 4) {
        const int a = value(input[i]), b = value(input[i + 1]);
        const int c = input[i + 2] == '=' ? -2 : value(input[i + 2]);
        const int d = input[i + 3] == '=' ? -2 : value(input[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1 ||
            (c == -2 && d != -2) || (i + 4 != input.size() && (c < 0 || d < 0)))
            return std::nullopt;
        output.push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
        if (c >= 0) output.push_back(static_cast<uint8_t>((b << 4) | (c >> 2)));
        if (d >= 0) output.push_back(static_cast<uint8_t>((c << 6) | d));
    }
    static constexpr uint8_t kPngSignature[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    if (output.size() < sizeof(kPngSignature) ||
        !std::equal(std::begin(kPngSignature), std::end(kPngSignature),
                    output.begin())) return std::nullopt;
    return output;
}
} // namespace

AmapClassicAssets::AmapClassicAssets(Engine& engine,
                                     PlatformBridge& platformBridge,
                                     Credentials credentials)
    : engine_(engine), platformBridge_(platformBridge),
      credentials_(std::move(credentials)) {}

AmapClassicAssets::~AmapClassicAssets() {
    {
        std::lock_guard<std::mutex> gate(callbackGate_->mutex);
        callbackGate_->alive = false;
    }
    reset();
}

std::chrono::seconds AmapClassicAssets::retryDelay(int attempt) {
    return std::chrono::seconds(1 << std::clamp(attempt - 1, 0, 2));
}

void AmapClassicAssets::requireGlyph(uint32_t codepoint) {
    if (codepoint == 0 || codepoint > 0x10ffff) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (installedGlyphs_.count(codepoint) || inFlightGlyphs_.count(codepoint))
        return;
    requiredGlyphs_.insert(codepoint);
    if (!demandTicket_) demandTicket_.emplace(WorkLedger::shared().acquire(
        WorkLedger::Kind::Landing, "amapOfficialAssetDemand"));
}

void AmapClassicAssets::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = true;
}

void AmapClassicAssets::requireAtlas(int atlas) {
    if (atlas <= 0) return;
    const auto officialAtlases = amapClassicPoiIconAtlases();
    if (std::find(officialAtlases.begin(), officialAtlases.end(), atlas) ==
        officialAtlases.end()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (installed_.count(atlas) != 0) return;
    requiredAtlases_.insert(atlas);
    if (!demandTicket_) {
        demandTicket_.emplace(WorkLedger::shared().acquire(
            WorkLedger::Kind::Landing, "amapOfficialIconDemand"));
    }
}

void AmapClassicAssets::installManifest(
    std::string version, std::string path, std::string type) {
    if (version.empty() || path.empty() || type.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    iconVersion_ = std::move(version);
    iconPath_ = std::move(path);
    iconType_ = std::move(type);
}

bool AmapClassicAssets::requestOneAtlas(
    SceneFrameResourceArbiter& resourceArbiter) {
    int atlas = 0;
    uint64_t id = 0;
    uint64_t generation = 0;
    std::string url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || iconVersion_.empty()) return false;
        const auto now = std::chrono::steady_clock::now();
        std::vector<int> candidates(requiredAtlases_.begin(),
                                    requiredAtlases_.end());
        std::sort(candidates.begin(), candidates.end());
        for (int candidate : candidates) {
            if (installed_.count(candidate) ||
                inFlightAtlases_.count(candidate) ||
                attempts_[candidate] >= 3 ||
                now < nextRetry_[candidate]) continue;
            atlas = candidate;
            break;
        }
        if (atlas == 0 || !resourceArbiter.tryAcquire(
                SceneFrameResourceProducer::Mvt,
                SceneFrameResourceStage::NetworkRequest,
                FrameResourcePriority::Normal)) return false;
        ++attempts_[atlas];
        inFlightAtlases_.insert(atlas);
        id = nextId_++;
        generation = generation_;
        const std::string iconBase = credentials_.iconBase.empty()
            ? "https://o4.amap.com/icon" : credentials_.iconBase;
        url = iconBase + "/" + iconVersion_ + "/" + iconPath_ + "/" +
              iconType_ + "/icons_" + std::to_string(atlas) +
              "?key=" + credentials_.webKey;
    }
    const auto callbackGate = callbackGate_;
    auto landing = std::make_shared<WorkLedger::Ticket>(
        WorkLedger::shared().acquire(
            WorkLedger::Kind::Landing, "amapOfficialAssetAtlas"));
    auto handle = platformBridge_.get(
        url,
        [this, callbackGate, landing, atlas, id, generation](
            int status, std::vector<uint8_t> body) {
            std::lock_guard<std::mutex> callbackLock(callbackGate->mutex);
            if (!callbackGate->alive) return;
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_) return;
            inFlightAtlases_.erase(atlas);
            if (status == 200 && !body.empty()) {
                landed_.push_back({atlas, std::move(body)});
            } else {
                nextRetry_[atlas] = std::chrono::steady_clock::now() +
                                    retryDelay(attempts_[atlas]);
            }
            completed_.push_back(id);
        },
        HttpRequestOptions(HttpRequestPriority::Low,
                           {{"Referer", kAmapOfficialReferer}}));
    std::lock_guard<std::mutex> lock(mutex_);
    const bool completedInline =
        std::find(completed_.begin(), completed_.end(), id) != completed_.end();
    if (generation == generation_ && !completedInline)
        requests_[id] = std::move(handle);
    return true;
}

bool AmapClassicAssets::requestOneGlyphBatch(
    SceneFrameResourceArbiter& resourceArbiter) {
    std::vector<uint32_t> batch;
    uint64_t id = 0, generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return false;
        std::vector<uint32_t> candidates(requiredGlyphs_.begin(),
                                         requiredGlyphs_.end());
        std::sort(candidates.begin(), candidates.end());
        for (uint32_t cp : candidates) {
            if (installedGlyphs_.count(cp) || inFlightGlyphs_.count(cp) ||
                glyphAttempts_[cp] >= 3) continue;
            batch.push_back(cp);
            if (batch.size() == 128) break;
        }
        if (batch.empty() || !resourceArbiter.tryAcquire(
                SceneFrameResourceProducer::Mvt,
                SceneFrameResourceStage::NetworkRequest,
                FrameResourcePriority::Normal)) return false;
        for (uint32_t cp : batch) {
            ++glyphAttempts_[cp];
            inFlightGlyphs_.insert(cp);
        }
        id = nextId_++;
        generation = generation_;
    }
    const std::string sdfBase = credentials_.sdfBase.empty()
        ? "https://sdf.amap.com/getsdfdata" : credentials_.sdfBase;
    std::string url = sdfBase + "?chars=";
    for (size_t i = 0; i < batch.size(); ++i) {
        if (i) url.push_back('|');
        url += std::to_string(batch[i]);
    }
    const auto callbackGate = callbackGate_;
    auto landing = std::make_shared<WorkLedger::Ticket>(
        WorkLedger::shared().acquire(WorkLedger::Kind::Landing,
                                    "amapOfficialSdfBatch"));
    auto handle = platformBridge_.get(
        url, [this, callbackGate, landing, batch, id, generation](
            int status, std::vector<uint8_t> body) {
            std::lock_guard<std::mutex> callbackLock(callbackGate->mutex);
            if (!callbackGate->alive) return;
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_) return;
            for (uint32_t cp : batch) inFlightGlyphs_.erase(cp);
            if (status == 200 && !body.empty()) {
                landedGlyphs_.push_back({batch, std::move(body)});
            } else {
                for (uint32_t cp : batch) {
                    if (glyphAttempts_[cp] >= 3) {
                        requiredGlyphs_.erase(cp);
                    }
                }
            }
            completed_.push_back(id);
        }, HttpRequestOptions(HttpRequestPriority::Low,
                              {{"Referer", kAmapOfficialReferer}}));
    std::lock_guard<std::mutex> lock(mutex_);
    const bool completedInline =
        std::find(completed_.begin(), completed_.end(), id) != completed_.end();
    if (generation == generation_ && !completedInline)
        requests_[id] = std::move(handle);
    return true;
}

void AmapClassicAssets::update(
    SceneFrameResourceArbiter& resourceArbiter) {
    Download download;
    GlyphDownload glyphDownload;
    bool hasDownload = false;
    bool hasGlyphDownload = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint64_t id : completed_) requests_.erase(id);
        completed_.clear();
        if (!landed_.empty() && resourceArbiter.tryAcquire(
                SceneFrameResourceProducer::Mvt,
                SceneFrameResourceStage::GpuUpload,
                FrameResourcePriority::Normal)) {
            download = std::move(landed_.front());
            landed_.erase(landed_.begin());
            hasDownload = true;
        }
        if (!landedGlyphs_.empty() && resourceArbiter.tryAcquire(
                SceneFrameResourceProducer::Mvt,
                SceneFrameResourceStage::GpuUpload,
                FrameResourcePriority::Normal)) {
            glyphDownload = std::move(landedGlyphs_.front());
            landedGlyphs_.erase(landedGlyphs_.begin());
            hasGlyphDownload = true;
        }
    }
    if (hasGlyphDownload) {
        bool complete = false;
        std::vector<GlyphAtlas::ProviderGlyph> glyphs;
        try {
            const auto json = nlohmann::json::parse(glyphDownload.body);
            if (json.value("code", 0) == 1 && json["url"].is_string() &&
                json["info"].is_object()) {
                auto png = decodePngDataUrl(json["url"].get<std::string>());
                auto image = png ? platformBridge_.decodeImage(
                    png->data(), png->size()) : nullptr;
                if (image && image->channels == 4) {
                    for (uint32_t cp : glyphDownload.codepoints) {
                        const auto it = json["info"].find(std::to_string(cp));
                        if (it == json["info"].end() || !it->is_array() ||
                            it->size() != 7) { glyphs.clear(); break; }
                        std::array<int, 7> v{};
                        bool valid = true;
                        for (size_t i = 0; i < v.size(); ++i) {
                            if (!(*it)[i].is_number_integer()) { valid = false; break; }
                            v[i] = (*it)[i].get<int>();
                        }
                        if (!valid || v[0] <= 0 || v[1] <= 0 || v[4] < 0 ||
                            v[5] < 0 || v[6] < 0 || v[5] + v[0] > image->width ||
                            v[6] + v[1] > image->height) { glyphs.clear(); break; }
                        glyphs.push_back({cp, v[0], v[1], v[2], v[3], v[4],
                                          v[5], v[6]});
                    }
                    if (glyphs.size() == glyphDownload.codepoints.size()) {
                        std::vector<uint8_t> gray(
                            static_cast<size_t>(image->width) * image->height);
                        for (size_t i = 0; i < gray.size(); ++i)
                            gray[i] = image->pixels[i * 4];
                        complete = engine_.installAmapClassicOfficialGlyphBatch(
                            image->width, image->height, gray, glyphs);
                    }
                }
            }
        } catch (...) { complete = false; }
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint32_t cp : glyphDownload.codepoints) {
            if (complete) {
                installedGlyphs_.insert(cp);
                requiredGlyphs_.erase(cp);
                glyphAttempts_.erase(cp);
            } else {
                if (glyphAttempts_[cp] >= 3) {
                    requiredGlyphs_.erase(cp);
                }
            }
        }
    }
    if (hasDownload) {
        const bool complete = installAtlas(std::move(download));
        std::lock_guard<std::mutex> lock(mutex_);
        if (complete) {
            installed_.insert(download.atlas);
            requiredAtlases_.erase(download.atlas);
            attempts_.erase(download.atlas);
            nextRetry_.erase(download.atlas);
        } else {
            nextRetry_[download.atlas] = std::chrono::steady_clock::now() +
                                         retryDelay(attempts_[download.atlas]);
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (requiredAtlases_.empty() && landed_.empty() &&
            inFlightAtlases_.empty() && requiredGlyphs_.empty() &&
            landedGlyphs_.empty() && inFlightGlyphs_.empty())
            demandTicket_.reset();
    }
    requestOneAtlas(resourceArbiter);
    requestOneGlyphBatch(resourceArbiter);
}

bool AmapClassicAssets::installAtlas(Download download) {
    auto image = platformBridge_.decodeImage(download.body.data(),
                                             download.body.size());
    const auto needed = amapClassicPoiIconFrames(download.atlas);
    if (!image || image->channels != 4 || needed.empty()) return false;

    struct PreparedFrame {
        std::string name;
        int width = 0;
        int height = 0;
        std::vector<uint8_t> pixels;
    };
    std::vector<PreparedFrame> prepared;
    prepared.reserve(needed.size());
    for (const auto& frameContract : needed) {
        if (image->width != frameContract.atlasWidth ||
            image->height != frameContract.atlasHeight ||
            frameContract.cellWidth <= 0 ||
            frameContract.cellHeight <= 0) return false;
        const int columns = image->width / frameContract.cellWidth;
        const int zeroBased = frameContract.iconIndex - 1;
        const int x0 = columns > 0
            ? (zeroBased % columns) * frameContract.cellWidth : -1;
        const int y0 = columns > 0
            ? (zeroBased / columns) * frameContract.cellHeight : -1;
        if (zeroBased < 0 || x0 < 0 || y0 < 0 ||
            x0 + frameContract.cellWidth > image->width ||
            y0 + frameContract.cellHeight > image->height) return false;
        PreparedFrame frame;
        frame.name = "amap-icons-" + std::to_string(download.atlas) + "-" +
                     std::to_string(frameContract.iconIndex);
        frame.width = frameContract.cellWidth;
        frame.height = frameContract.cellHeight;
        frame.pixels.resize(static_cast<size_t>(frame.width) * frame.height * 4);
        for (int y = 0; y < frame.height; ++y) {
            const size_t src =
                (static_cast<size_t>(y0 + y) * image->width + x0) * 4;
            const size_t dst = static_cast<size_t>(y) * frame.width * 4;
            std::copy_n(image->pixels.data() + src, frame.width * 4,
                        frame.pixels.data() + dst);
        }
        prepared.push_back(std::move(frame));
    }
    for (const auto& frame : prepared) {
        if (!engine_.addOfficialIconImage(frame.name, frame.width, frame.height,
                                          frame.pixels)) return false;
    }
    return true;
}

void AmapClassicAssets::reset() {
    std::unordered_map<uint64_t, std::unique_ptr<HttpRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        requests.swap(requests_);
        completed_.clear();
        landed_.clear();
        landedGlyphs_.clear();
        installed_.clear();
        requiredAtlases_.clear();
        inFlightAtlases_.clear();
        attempts_.clear();
        nextRetry_.clear();
        started_ = false;
        requiredGlyphs_.clear();
        inFlightGlyphs_.clear();
        installedGlyphs_.clear();
        glyphAttempts_.clear();
        iconVersion_.clear();
        iconPath_.clear();
        iconType_.clear();
        demandTicket_.reset();
    }
    // HttpRequest destruction can synchronously complete cancellation; never
    // hold the lifecycle mutex while releasing handles whose callbacks lock it.
    requests.clear();
    engine_.clearAmapClassicOfficialAssets();
}

bool AmapClassicAssets::glyphsReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requiredGlyphs_.empty() && landedGlyphs_.empty() &&
           inFlightGlyphs_.empty();
}

bool AmapClassicAssets::iconsReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requiredAtlases_.empty() && landed_.empty() &&
           inFlightAtlases_.empty();
}

} // namespace earth_engine
