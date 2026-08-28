#pragma once

#include "RasterOverlayTile.h"
#include "../tiling/RasterOverlayProjection.h"
#include "../tiling/TileKey.h"
#include "../core/math/Rectangle.h"

#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace earth_engine {

struct DecodedImage;

/// Stable identity for one decoded raster source asset.
///
/// The identity deliberately contains provider instance, content revision,
/// depot epoch, scheme and projection.  A provider id alone is not sufficient:
/// two instances may use different credentials, styles or adapters.
struct RasterAssetKey {
    uint64_t providerInstance = 0;
    uint64_t providerContentRevision = 0;
    uint64_t depotEpoch = 0;
    SchemeId schemeId;
    RasterOverlayProjection projection = RasterOverlayProjection::Geographic;
    TileKey sourceKey{};

    bool operator==(const RasterAssetKey& rhs) const {
        return providerInstance == rhs.providerInstance &&
               providerContentRevision == rhs.providerContentRevision &&
               depotEpoch == rhs.depotEpoch && schemeId == rhs.schemeId &&
               projection == rhs.projection && sourceKey == rhs.sourceKey;
    }
    bool operator!=(const RasterAssetKey& rhs) const { return !(*this == rhs); }
};

/// Immutable, consumer-neutral view of a successfully decoded source image.
/// It intentionally exposes no transport, waiter or ancestor-fallback state.
struct RasterAssetSnapshot {
    RasterAssetKey key;
    TileKey resolvedKey{};
    Rectangle bounds;
    std::shared_ptr<const DecodedImage> image;
    std::vector<std::string> credits;
    RasterOverlayTile::MoreDetailAvailable moreDetailAvailable =
        RasterOverlayTile::MoreDetailAvailable::Unknown;
};

/// Internal source-result shape shared by Direct and PageStore consumers.
/// Unlike RasterAssetSnapshot it may carry a terminal failure and diagnostics,
/// which lets a PageStore source slot complete instead of remaining pending
/// forever after a malformed or failed transport.
struct RasterAssetResponse {
    std::optional<RasterAssetSnapshot> asset;
    std::vector<std::string> diagnostics;
    bool terminalFailure = false;
};

/// How a backend-neutral asset acquire was satisfied.
enum class RasterAssetAcquireStatus {
    CacheHit,
    JoinedInFlight,
    StartedTransport,
    AdmissionDenied
};

/// Owner-scoped lease on one shared raster source request.
///
/// Cancelling the lease detaches only this consumer. The shared transport and
/// decoded asset remain owned by the depot so a Direct or PageStore peer can
/// continue using the same work.
class RasterAssetRequestHandle {
public:
    RasterAssetRequestHandle() = default;
    ~RasterAssetRequestHandle() { cancel(); }

    RasterAssetRequestHandle(const RasterAssetRequestHandle&) = delete;
    RasterAssetRequestHandle& operator=(
        const RasterAssetRequestHandle&) = delete;

    RasterAssetRequestHandle(RasterAssetRequestHandle&& other) noexcept
        : state_(std::move(other.state_)) {}
    RasterAssetRequestHandle& operator=(
        RasterAssetRequestHandle&& other) noexcept {
        if (this != &other) {
            cancel();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    void cancel() {
        if (!state_ ||
            !state_->active.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        if (state_->detach) {
            state_->detach();
        }
    }

    bool active() const {
        return state_ && state_->active.load(std::memory_order_acquire);
    }

private:
    struct State {
        std::atomic<bool> active{true};
        std::function<void()> detach;
    };

    explicit RasterAssetRequestHandle(std::shared_ptr<State> state)
        : state_(std::move(state)) {}

    std::shared_ptr<State> state_;

    friend class RasterOverlayTileProvider;
};

struct RasterAssetAcquireResult {
    RasterAssetAcquireStatus status =
        RasterAssetAcquireStatus::AdmissionDenied;
    RasterAssetRequestHandle handle;

    bool accepted() const {
        return status != RasterAssetAcquireStatus::AdmissionDenied;
    }
};

} // namespace earth_engine

namespace std {
template <>
struct hash<earth_engine::RasterAssetKey> {
    size_t operator()(const earth_engine::RasterAssetKey& key) const noexcept {
        size_t h = std::hash<uint64_t>()(key.providerInstance);
        auto combine = [&h](size_t value) {
            h ^= value + static_cast<size_t>(0x9e3779b9u) + (h << 6) +
                 (h >> 2);
        };
        combine(std::hash<uint64_t>()(key.providerContentRevision));
        combine(std::hash<uint64_t>()(key.depotEpoch));
        combine(std::hash<earth_engine::SchemeId>()(key.schemeId));
        combine(std::hash<int>()(
            static_cast<int>(key.projection)));
        combine(std::hash<earth_engine::TileKey>()(key.sourceKey));
        return h;
    }
};
} // namespace std
