#pragma once

#include "../providers/RasterAssetDepot.h"

#include <memory>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class RenderDevice;
class RasterOverlayTileProvider;

enum class RasterOverlayBackendKind {
    Direct,
    PageStore
};

/// Phase-one replaceable provider-view strategy for one overlay consumer.
///
/// This is intentionally narrower than a full executor backend: Direct
/// selection/load/render and TerrainPageStore execution still live in their
/// existing production chains. The strategy may filter participating
/// providers, but Runtime normalizes the result back to canonical overlay
/// order and rejects providers outside the Runtime-owned stack.
class RasterOverlayBackend {
public:
    virtual ~RasterOverlayBackend() = default;

    virtual RasterOverlayBackendKind kind() const = 0;
    virtual const std::vector<RasterOverlayTileProvider*>& providers(
        const std::vector<ActivatedRasterOverlay*>& overlays,
        RenderDevice* device) = 0;
    virtual void setEnabled(bool enabled) = 0;
    virtual bool enabled() const = 0;
};

/// Tileset-owned runtime for the ordered raster overlay stack.
///
/// The runtime owns ordering and activation policy.  Direct mappedRaster and
/// TerrainPageStore consume the same overlay view, while backend selection can
/// evolve behind this boundary without changing RasterOverlay configuration.
class RasterOverlayRuntime {
public:
    explicit RasterOverlayRuntime(
        std::vector<ActivatedRasterOverlay*> overlays = {});
    ~RasterOverlayRuntime();

    RasterOverlayRuntime(const RasterOverlayRuntime&) = delete;
    RasterOverlayRuntime& operator=(const RasterOverlayRuntime&) = delete;

    const std::vector<ActivatedRasterOverlay*>& overlays() const {
        return overlays_;
    }
    std::vector<ActivatedRasterOverlay*>& overlays() { return overlays_; }

    /// Ensure all configured overlays have a provider and return the ordered
    /// provider stack. Null overlays/providers are omitted from the view.
    const std::vector<RasterOverlayTileProvider*>& ensureProviders(
        RenderDevice* device);

    /// Backend provider-view seam for the migration. Returned providers are
    /// always a canonical-order subset of overlays(); backend-provided order,
    /// duplicates, and foreign providers never change runtime slot identity.
    const std::vector<RasterOverlayTileProvider*>& providersForBackend(
        RasterOverlayBackendKind backend,
        RenderDevice* device);

    /// Install a phase-one provider-view strategy for one consumer. Passing
    /// nullptr restores the default strategy. This does not yet replace the
    /// consumer's selection/load/render executor; that requires the future
    /// frame-level RasterBindingSet boundary.
    bool setBackend(RasterOverlayBackendKind kind,
                    std::unique_ptr<RasterOverlayBackend> backend);

    const RasterOverlayBackend* backend(RasterOverlayBackendKind kind) const;

    /// Shared decoded-source access point used by every backend in this
    /// tileset. Provider-specific caches stay behind this consumer-neutral
    /// boundary during the first migration phase.
    RasterAssetDepot& assetDepot() { return *assetDepot_; }
    const RasterAssetDepot& assetDepot() const { return *assetDepot_; }
    std::shared_ptr<RasterAssetDepot> assetDepotHandle() const {
        return assetDepot_;
    }

    /// Enable/disable one backend without changing overlay ownership. A
    /// disabled PageStore returns an empty provider view, which makes its
    /// consumer fail closed and preserve Direct mappedRaster fallback.
    void setBackendEnabled(RasterOverlayBackendKind backend, bool enabled);
    bool backendEnabled(RasterOverlayBackendKind backend) const;

private:
    std::vector<ActivatedRasterOverlay*> overlays_;
    std::shared_ptr<RasterAssetDepot> assetDepot_;
    std::unique_ptr<RasterOverlayBackend> directBackend_;
    std::unique_ptr<RasterOverlayBackend> pageStoreBackend_;
    std::vector<RasterOverlayTileProvider*> directProviders_;
    std::vector<RasterOverlayTileProvider*> pageStoreProviders_;
};

} // namespace earth_engine
