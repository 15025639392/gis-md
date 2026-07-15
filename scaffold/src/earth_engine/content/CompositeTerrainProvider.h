#pragma once

#include "GltfContentProvider.h"
#include "../tiling/TileKey.h"

#include <memory>
#include <string>
#include <vector>

namespace earth_engine {

/// Fuses a primary terrain provider (e.g. quantized-mesh, partial coverage) with
/// an ellipsoid fallback provider so that regions the primary source does not
/// cover render as a smooth ellipsoid surface instead of stopping at a coarse
/// leaf tile with a giant skirt wall.
///
/// The whole behaviour is driven by availabilityState: within
/// [0, ellipsoidMaxZoom] any tile the primary source lacks reports Available
/// (source = ellipsoid), so TileChildMaterializer never hits the
/// "no child available" branch that leaves an exposed coarse leaf. Content
/// requests route per-tile to the primary source where it is available, else to
/// the ellipsoid provider. See docs/issues/no-fine-data-ellipsoid-fallback-
/// design-2026-07-07.md (Approach A).
///
/// Both wrapped providers must share the same tiling scheme; the caller
/// constructs the ellipsoid provider with the primary provider's schemeId.
class CompositeTerrainProvider final : public TilesetContentProvider {
public:
    CompositeTerrainProvider(
        std::unique_ptr<TilesetContentProvider> primary,
        std::unique_ptr<TilesetContentProvider> ellipsoid,
        int ellipsoidMaxZoom);

    std::string id() const override;
    bool supportsTile(const TileKey& key) const override;
    std::vector<TileKey> rootTiles() const override;
    std::optional<TilesetContentTileMetadata> tileMetadata(
        const TileKey& key) const override;
    std::vector<TileKey> childTiles(const TileKey& key) const override;
    bool providesTerrainQuadtree() const override { return true; }
    TileAvailabilityState availabilityState(
        const TileKey& key) const override;
    uint64_t childTopologyRevision() const override;
    bool isTerrainAvailabilityBoundaryLevel(int level) const override;
    void applyAvailabilityUpdates(
        const std::vector<QuantizedMeshAvailabilityUpdate>& updates) override;
    int estimatedRequestFanout(const TileKey& key) const override;

    void requestTileContent(const TileKey& key,
                            CancellationToken token,
                            ContentCallback callback,
                            HttpRequestPriority priority =
                                HttpRequestPriority::Normal) override;
    void requestTileContent(const TileKey& key,
                            CancellationToken token,
                            ContentCallback callback,
                            HttpRequestPriority priority,
                            TileContentRequestOptions options) override;
    TileContentLoadResult decodeContent(const uint8_t* data,
                                        size_t size) override;
    ProviderRequestDiagnostics requestDiagnostics() const override;

private:
    /// True when the primary source actually covers this tile (routes content to
    /// the primary source and keeps its uncapped availability).
    bool primaryAvailable(const TileKey& key) const;

    /// True when NONE of this tile's four siblings has primary coverage, i.e. the
    /// parent would otherwise stop as a coarse leaf (the giant-skirt case). Only
    /// then does the ellipsoid floor apply: at a coverage edge some sibling IS
    /// covered, so the uncovered ones must stay real-terrain upsample tiles
    /// (clipped from the covered parent) rather than flatten to the ellipsoid.
    bool isPureHoleQuad(const TileKey& key) const;

    std::unique_ptr<TilesetContentProvider> primary_;
    std::unique_ptr<TilesetContentProvider> ellipsoid_;
    int ellipsoidMaxZoom_ = 0;
};

} // namespace earth_engine
