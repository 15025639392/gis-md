#pragma once

#include "../core/async/WorkLedger.h"
#include "../data/Feature.h"
#include "../threading/CancellationToken.h"
#include "../tiling/TileKey.h"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace earth_engine {

class AmapSurfaceMaskStyleState;

/// Shared CPU cache for official AMap terrain fill pages.
///
/// This is deliberately not an ImageryProvider: a request is keyed by the
/// selected terrain footprint and has no ancestor/placeholder fallback. The
/// returned RGBA page is uploaded by the tile that owns the footprint.
class AmapTerrainFillMaskStore final {
public:
    using FeatureSet = std::shared_ptr<const std::vector<Feature>>;
    using FeatureFetchCallback = std::function<void(FeatureSet)>;
    using FeatureFetch = std::function<void(
        const TileKey&, CancellationToken, FeatureFetchCallback)>;

    struct Result {
        std::shared_ptr<const std::vector<uint8_t>> pixels;
        uint64_t revision = 0;
        bool pending = false;
        bool failed = false;
    };

    /// Diagnostics snapshot for the terrain-fill mask request probe.  Counts
    /// are since the last takeProbe (or construction).  Intended for demo
    /// logcat only; no behavior is changed by reading it.
    struct Probe {
        uint64_t presentHits = 0;   ///< page already cached (same-frame upload)
        uint64_t asyncPending = 0;  ///< coalesced onto an in-flight fetch
        uint64_t startedFetches = 0;///< started a new shared type-1 fetch
        uint64_t failed = 0;        ///< returned failure (no page)
        size_t residentPages = 0;   ///< cached pages resident right now
        /// Last requested key's scheme / zoom, for source-mapping diagnostics.
        std::string lastScheme;
        int lastZ = -1;
    };
    Probe takeProbe();

    AmapTerrainFillMaskStore(
        FeatureFetch fetch,
        std::shared_ptr<AmapSurfaceMaskStyleState> styleState,
        size_t maximumResidentPages = 96);
    ~AmapTerrainFillMaskStore();

    AmapTerrainFillMaskStore(const AmapTerrainFillMaskStore&) = delete;
    AmapTerrainFillMaskStore& operator=(const AmapTerrainFillMaskStore&) = delete;

    /// Request a page for exactly `key`. A cache miss starts one coalesced
    /// fetch and returns pending; no parent page is ever returned.
    Result request(const TileKey& key);

    void setStyleState(std::shared_ptr<AmapSurfaceMaskStyleState> state);
    uint64_t styleRevision() const;
    double displayZoom() const;
    bool hasWorkInFlight() const;
    size_t residentPageCount() const;

private:
    struct State;
    struct Entry {
        std::shared_ptr<const std::vector<uint8_t>> pixels;
        uint64_t revision = 0;
        bool pending = false;
        bool failed = false;
        uint64_t lastAccess = 0;
        CancellationToken token;
        // Held by the entry as well as the async callback so cancellation or
        // eviction can release the Landing ticket even when the transport
        // never invokes its callback.
        std::shared_ptr<WorkLedger::Ticket> landingTicket;
    };

    static void pruneResidentPages(State& state,
                                   const TileKey* protectedKey = nullptr);

    std::shared_ptr<State> state_;
};

} // namespace earth_engine
