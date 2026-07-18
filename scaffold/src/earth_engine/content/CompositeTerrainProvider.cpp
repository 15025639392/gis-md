#include "CompositeTerrainProvider.h"

#include "../tiling/TileQuadtreeChildKeys.h"
#include "../tiling/TileSelectionRootPolicy.h"

#include <utility>

namespace earth_engine {

CompositeTerrainProvider::CompositeTerrainProvider(
    std::unique_ptr<TilesetContentProvider> primary,
    std::unique_ptr<TilesetContentProvider> ellipsoid,
    int ellipsoidMaxZoom)
    : primary_(std::move(primary)),
      ellipsoid_(std::move(ellipsoid)),
      ellipsoidMaxZoom_(ellipsoidMaxZoom) {}

bool CompositeTerrainProvider::primaryAvailable(const TileKey& key) const {
    return primary_->availabilityState(key) ==
           TileAvailabilityState::Available;
}

bool CompositeTerrainProvider::isPureHoleQuad(const TileKey& key) const {
    // z0 has no siblings within a parent quad; a level-zero root the primary
    // source lacks is itself a pure hole.
    if (key.z == 0) {
        return !primaryAvailable(key);
    }
    // Ellipsoid only fills where the whole sibling quad is uncovered (the parent
    // would stop as a coarse leaf). If any sibling is primary-covered this is a
    // coverage edge: the uncovered siblings must remain real-terrain upsample
    // tiles (materializeTerrainChildren marks them so), not flatten to ellipsoid.
    for (const TileKey& sibling :
         TileQuadtreeChildKeys::terrainChildren(key.parent())) {
        if (primaryAvailable(sibling)) {
            return false;
        }
    }
    return true;
}

std::string CompositeTerrainProvider::id() const {
    return "composite-terrain(" + primary_->id() + "+" + ellipsoid_->id() +
           ")";
}

bool CompositeTerrainProvider::supportsTile(const TileKey& key) const {
    return primary_->supportsTile(key) || ellipsoid_->supportsTile(key);
}

std::vector<TileKey> CompositeTerrainProvider::rootTiles() const {
    // Both providers share the scheme and return the same virtual terrain root.
    return primary_->rootTiles();
}

std::optional<TilesetContentTileMetadata>
CompositeTerrainProvider::tileMetadata(const TileKey& key) const {
    // Prefer the primary source (real height range + bounding volume); fall back
    // to the ellipsoid (h=0 region, geometric-error seed) in pure-hole regions.
    if (auto metadata = primary_->tileMetadata(key)) {
        return metadata;
    }
    return ellipsoid_->tileMetadata(key);
}

std::vector<TileKey> CompositeTerrainProvider::childTiles(
    const TileKey& key) const {
    // The primary source yields children wherever it is covered (including deep
    // levels past the ellipsoid cap); the ellipsoid is the global superset that
    // fills holes up to the cap.
    if (auto children = primary_->childTiles(key); !children.empty()) {
        return children;
    }
    return ellipsoid_->childTiles(key);
}

TileAvailabilityState CompositeTerrainProvider::availabilityState(
    const TileKey& key) const {
    // Primary source availability is never capped: a covered deep tile stays
    // Available even below the ellipsoid floor.
    if (primaryAvailable(key)) {
        return TileAvailabilityState::Available;
    }
    // supportsTile guards the tiling scheme and the ellipsoid cap (its own
    // maximumLevel). The virtual root is always fillable.
    if (!ellipsoid_->supportsTile(key)) {
        return TileAvailabilityState::NotAvailable;
    }
    if (TileSelectionRootPolicy::isVirtualTerrainRoot(key)) {
        return TileAvailabilityState::Available;
    }
    // Ellipsoid floor is intentionally narrow: it only fills a fully-uncovered
    // sibling quad (where the parent would otherwise stop as a coarse leaf with
    // a giant skirt). At a coverage edge the uncovered siblings stay NotAvailable
    // so they become real-terrain upsample tiles, not flat ellipsoid.
    if (key.z <= ellipsoidMaxZoom_ && isPureHoleQuad(key)) {
        return TileAvailabilityState::Available;
    }
    return TileAvailabilityState::NotAvailable;
}

uint64_t CompositeTerrainProvider::childTopologyRevision() const {
    return primary_->childTopologyRevision();
}

bool CompositeTerrainProvider::isTerrainAvailabilityBoundaryLevel(
    int level) const {
    return primary_->isTerrainAvailabilityBoundaryLevel(level);
}

int CompositeTerrainProvider::estimatedRequestFanout(
    const TileKey& key) const {
    // Ellipsoid content resolves synchronously with no network fanout.
    return primaryAvailable(key) ? primary_->estimatedRequestFanout(key) : 1;
}

void CompositeTerrainProvider::requestTileContent(
    const TileKey& key,
    CancellationToken token,
    ContentCallback callback,
    HttpRequestPriority priority) {
    if (primaryAvailable(key)) {
        primary_->requestTileContent(
            key, std::move(token), std::move(callback), priority);
    } else {
        ellipsoid_->requestTileContent(
            key, std::move(token), std::move(callback), priority);
    }
}

void CompositeTerrainProvider::requestTileContent(
    const TileKey& key,
    CancellationToken token,
    ContentCallback callback,
    HttpRequestPriority priority,
    TileContentRequestOptions options) {
    if (primaryAvailable(key)) {
        primary_->requestTileContent(
            key, std::move(token), std::move(callback), priority, options);
    } else {
        ellipsoid_->requestTileContent(
            key, std::move(token), std::move(callback), priority, options);
    }
}

TileContentLoadResult CompositeTerrainProvider::decodeContent(
    const uint8_t* data,
    size_t size) {
    // Only the primary source emits encoded bytes; ellipsoid content is built
    // inline in requestTileContent and never decoded.
    return primary_->decodeContent(data, size);
}

ProviderRequestDiagnostics CompositeTerrainProvider::requestDiagnostics()
    const {
    return primary_->requestDiagnostics();
}

} // namespace earth_engine
