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
