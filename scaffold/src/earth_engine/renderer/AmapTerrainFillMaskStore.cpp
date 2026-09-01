#include "AmapTerrainFillMaskStore.h"

#include "../core/async/WorkLedger.h"
#include "../data/AmapSurfaceMaskRasterizer.h"
#include "../providers/AmapSurfaceMaskImageryProvider.h"
#include "../style/AmapClassicStyleInternal.h"
#include "../tiling/TileScheme.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

namespace earth_engine {

struct AmapTerrainFillMaskStore::State {
    FeatureFetch fetch;
    mutable std::mutex mutex;
    std::shared_ptr<AmapSurfaceMaskStyleState> styleState;
    uint64_t styleIdentity = 1;
    uint64_t accessClock = 0;
    size_t maximumResidentPages = 96;
    std::unordered_map<TileKey, Entry> entries;
    std::atomic<bool> alive{true};
};

namespace {

uint64_t composedRevision(
    uint64_t styleIdentity,
    const AmapSurfaceMaskStyleState::Snapshot& snapshot) {
    return (styleIdentity << 32u) | (snapshot.revision & 0xFFFFFFFFu);
}

} // namespace

void AmapTerrainFillMaskStore::pruneResidentPages(
    State& state, const TileKey* protectedKey) {
    while (state.entries.size() > state.maximumResidentPages) {
        auto victim = state.entries.end();
        uint64_t oldest = std::numeric_limits<uint64_t>::max();
        for (auto it = state.entries.begin(); it != state.entries.end(); ++it) {
            if (protectedKey && it->first == *protectedKey) {
                continue;
            }
            if (it->second.lastAccess < oldest) {
                oldest = it->second.lastAccess;
                victim = it;
            }
        }
        if (victim == state.entries.end()) break;
        if (victim->second.pending) {
            victim->second.token.cancel();
            if (victim->second.landingTicket) {
                victim->second.landingTicket->release();
            }
        }
        state.entries.erase(victim);
    }
}

AmapTerrainFillMaskStore::AmapTerrainFillMaskStore(
    FeatureFetch fetch,
    std::shared_ptr<AmapSurfaceMaskStyleState> styleState,
    size_t maximumResidentPages)
    : state_(std::make_shared<State>()) {
    state_->fetch = std::move(fetch);
    state_->styleState = std::move(styleState);
    state_->maximumResidentPages = std::max<size_t>(1, maximumResidentPages);
}

AmapTerrainFillMaskStore::~AmapTerrainFillMaskStore() {
    const std::shared_ptr<State> state = state_;
    state->alive.store(false, std::memory_order_release);
    std::vector<std::shared_ptr<WorkLedger::Ticket>> tickets;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        for (auto& [key, entry] : state->entries) {
            (void)key;
            if (entry.pending) entry.token.cancel();
            if (entry.landingTicket) {
                tickets.push_back(entry.landingTicket);
                entry.landingTicket.reset();
            }
        }
    }
    for (const auto& ticket : tickets) {
        if (ticket) ticket->release();
    }
}

uint64_t AmapTerrainFillMaskStore::styleRevision() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const AmapSurfaceMaskStyleState::Snapshot snapshot =
        state_->styleState ? state_->styleState->snapshot()
                           : AmapSurfaceMaskStyleState::Snapshot{};
    return composedRevision(state_->styleIdentity, snapshot);
}

double AmapTerrainFillMaskStore::displayZoom() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->styleState ? state_->styleState->displayZoom() : 0.0;
}

void AmapTerrainFillMaskStore::setStyleState(
    std::shared_ptr<AmapSurfaceMaskStyleState> state) {
    std::vector<std::shared_ptr<WorkLedger::Ticket>> tickets;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->styleState = std::move(state);
        ++state_->styleIdentity;
        for (auto& [key, entry] : state_->entries) {
            (void)key;
            if (entry.pending) entry.token.cancel();
            if (entry.landingTicket) {
                tickets.push_back(entry.landingTicket);
                entry.landingTicket.reset();
            }
        }
        state_->entries.clear();
    }
    for (const auto& ticket : tickets) {
        if (ticket) ticket->release();
    }
}

bool AmapTerrainFillMaskStore::hasWorkInFlight() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    for (const auto& [key, entry] : state_->entries) {
        (void)key;
        if (entry.pending) return true;
    }
    return false;
}

size_t AmapTerrainFillMaskStore::residentPageCount() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    size_t count = 0;
    for (const auto& [key, entry] : state_->entries) {
        (void)key;
        count += entry.pixels ? 1u : 0u;
    }
    return count;
}

AmapTerrainFillMaskStore::Result AmapTerrainFillMaskStore::request(
    const TileKey& key) {
    const std::shared_ptr<State> state = state_;
    if (!state->fetch) return Result{nullptr, 0, false, true};

    std::unique_ptr<TileScheme> targetScheme;
    if (key.schemeId == "Geographic-TMS") {
        targetScheme = TileScheme::createGeographicTMS();
    } else if (key.schemeId == "XYZ-WebMercator") {
        targetScheme = TileScheme::createXYZWebMercator();
    } else {
        return Result{nullptr, 0, false, true};
    }
    if (key.z < 0 || key.z > 25 || key.x < 0 || key.y < 0 ||
        key.x >= targetScheme->tileCountX(key.z) ||
        key.y >= targetScheme->tileCountY(key.z)) {
        return Result{nullptr, 0, false, true};
    }

    AmapSurfaceMaskStyleState::Snapshot styleSnapshot;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->styleState) styleSnapshot = state->styleState->snapshot();
        styleSnapshot.revision = composedRevision(
            state->styleIdentity, styleSnapshot);
    }
    const uint64_t revision = styleSnapshot.revision;
    const double displayZoomValue = styleSnapshot.displayZoom;
    CancellationToken token;
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        Entry& entry = state->entries[key];
        entry.lastAccess = ++state->accessClock;
        pruneResidentPages(*state, &key);
        if (entry.revision == revision && entry.pixels) {
            return Result{entry.pixels, entry.revision, false, false};
        }
        if (entry.pending) {
            if (entry.revision == revision) {
                return Result{nullptr, revision, true, false};
            }
            // A discrete style change must not wait behind work that can only
            // produce a stale page. Cancel that generation and immediately
            // reuse the exact tile entry for the new revision.
            entry.token.cancel();
            entry.pending = false;
        }
        if (entry.failed && entry.revision == revision) {
            entry.failed = false;
            return Result{nullptr, revision, false, true};
        }
        entry.pending = true;
        entry.failed = false;
        entry.revision = revision;
        entry.token = CancellationToken{};
        token = entry.token;
        start = true;
    }
    if (start) {
        // The decoded Type-1 cache may complete on a transport/decode worker.
        // Hold one Landing ticket per exact selected-tile request until the
        // page result has been published into this store.  Its release wakes
        // an on-demand render loop so the ready RGBA page can be uploaded;
        // holding it does not burn frames while the worker is in flight.
        auto landingTicket = std::make_shared<WorkLedger::Ticket>(
            WorkLedger::shared().acquire(
                WorkLedger::Kind::Landing,
                "amapTerrainFillMaskFetch"));
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            auto it = state->entries.find(key);
            if (it == state->entries.end() || !it->second.pending ||
                it->second.revision != revision ||
                !it->second.token.sharesStateWith(token)) {
                landingTicket->release();
                return Result{nullptr, revision, false, true};
            }
            it->second.landingTicket = landingTicket;
        }
        state->fetch(key, token,
               [state, key, revision, displayZoomValue,
                token, landingTicket](FeatureSet features) {
                   if (!state->alive.load(std::memory_order_acquire)) {
                       landingTicket->release();
                       return;
                   }
                   std::shared_ptr<const std::vector<uint8_t>> pixels;
                   if (features) {
                       auto targetScheme = key.schemeId == "Geographic-TMS"
                           ? TileScheme::createGeographicTMS()
                           : TileScheme::createXYZWebMercator();
                       auto image = makeAmapSurfaceMaskImage(
                           features,
                           targetScheme->tileToRectangle(key),
                           displayZoomValue,
                           key.schemeId == "XYZ-WebMercator"
                               ? AmapSurfaceMaskRasterizerOptions::Projection::WebMercator
                               : AmapSurfaceMaskRasterizerOptions::Projection::Geographic);
                       if (image && !image->pixels.empty()) {
                           pixels = std::make_shared<const std::vector<uint8_t>>(
                               std::move(image->pixels));
                       }
                   }
                   {
                       std::lock_guard<std::mutex> lock(state->mutex);
                       auto it = state->entries.find(key);
                       if (it != state->entries.end() &&
                           it->second.pending &&
                           it->second.revision == revision &&
                           it->second.token.sharesStateWith(token)) {
                           it->second.pending = false;
                           it->second.failed = !pixels;
                           it->second.pixels = std::move(pixels);
                           it->second.revision = revision;
                           it->second.lastAccess = ++state->accessClock;
                           it->second.landingTicket.reset();
                           pruneResidentPages(*state, &key);
                       }
                   }
                   // Publish first, wake second: the frame caused by this
                   // Landing release must already be able to observe ready
                   // pixels (or the terminal failure) without polling.
                   landingTicket->release();
               });
    }
    // The shared decoded cache is allowed to satisfy the request inline.
    // Observe that result before reporting pending so a local hit can upload
    // in this same render frame.
    std::lock_guard<std::mutex> lock(state->mutex);
    auto it = state->entries.find(key);
    if (it == state->entries.end() || it->second.revision != revision) {
        return Result{nullptr, revision, false, true};
    }
    it->second.lastAccess = ++state->accessClock;
    if (it->second.pixels) {
        return Result{it->second.pixels, revision, false, false};
    }
    if (it->second.failed) {
        it->second.failed = false;
        return Result{nullptr, revision, false, true};
    }
    return Result{nullptr, revision, it->second.pending, false};
}

} // namespace earth_engine
