#pragma once

#include "../providers/RasterAssetDepot.h"
#include "../layers/RasterOverlay.h"

#include <memory>
#include <cstdint>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class RenderDevice;
class RasterOverlayTileProvider;

enum class RasterOverlayBackendKind {
    Direct,
    PageStore
};

struct RasterOverlayFrameSlot {
    size_t runtimeSlot = 0;
    ActivatedRasterOverlay* overlay = nullptr;
    RasterOverlayTileProvider* directProvider = nullptr;
    RasterOverlayTileProvider* pageStoreProvider = nullptr;

    // Value snapshot for the published frame. Consumers must use these
    // fields instead of rereading mutable RasterOverlay options after
    // beginFrame(). This is the contract that keeps Direct and PageStore
    // decisions identical when presentation options change mid-frame.
    bool visible = true;
    float opacity = 1.0f;
    RasterOverlayRole role = RasterOverlayRole::BaseImagery;
    RasterOverlayPriority priority = RasterOverlayPriority::High;
    RasterOverlayFallbackPolicy fallbackPolicy =
        RasterOverlayFallbackPolicy::AncestorOrPlaceholder;
    bool blocksCompleteRenderable = true;
    RasterOverlayProjection projection = RasterOverlayProjection::Geographic;
    uint64_t providerRevision = 0;
    uint64_t generation = 0;
};

/// Frame-frozen source view consumed by PageStore.  The provider pointer is
/// still owned by ActivatedRasterOverlay; all presentation and domain
/// metadata in this record belongs to the published frame and must not be
/// reread from the mutable overlay while a page bake is in flight.
struct RasterOverlayPageSource {
    size_t runtimeSlot = 0;
    RasterOverlayTileProvider* provider = nullptr;
    float opacity = 1.0f;
    RasterOverlayRole role = RasterOverlayRole::BaseImagery;
    RasterOverlayPriority priority = RasterOverlayPriority::High;
    RasterOverlayFallbackPolicy fallbackPolicy =
        RasterOverlayFallbackPolicy::AncestorOrPlaceholder;
    bool blocksCompleteRenderable = true;
    RasterOverlayProjection projection = RasterOverlayProjection::Geographic;
    uint64_t providerRevision = 0;
};

/// Immutable-for-the-frame execution snapshot shared by every raster stage.
///
/// directOverlays() always preserves the Runtime-owned slot count and order;
/// a backend-filtered slot is represented by nullptr instead of compacting the
/// vector. This keeps TileRasterOverlayState mapping indices, generated
/// texcoord identities and render binding order coherent across the frame.
class RasterOverlayFrameContext {
public:
    uint64_t frameNumber() const { return frameNumber_; }
    uint64_t generation() const { return generation_; }
    uint64_t directGeneration() const { return directGeneration_; }
    uint64_t pageStoreGeneration() const { return pageStoreGeneration_; }

    const std::vector<RasterOverlayFrameSlot>& slots() const {
        return slots_;
    }
    const std::vector<ActivatedRasterOverlay*>& directOverlays() const {
        return directOverlays_;
    }
    const std::vector<RasterOverlayTileProvider*>& directProviders() const {
        return directProviders_;
    }
    const std::vector<RasterOverlayPageSource>& pageStoreSources() const {
        return pageStoreSources_;
    }
    bool pageStoreHasDirectFallbackParity() const {
        return pageStoreHasDirectFallbackParity_;
    }
    std::shared_ptr<RasterAssetDepot> assetDepotHandle() const {
        return assetDepot_;
    }

private:
    uint64_t frameNumber_ = 0;
    uint64_t generation_ = 0;
    uint64_t directGeneration_ = 0;
    uint64_t pageStoreGeneration_ = 0;
    std::vector<RasterOverlayFrameSlot> slots_;
    std::vector<ActivatedRasterOverlay*> directOverlays_;
    std::vector<RasterOverlayTileProvider*> directProviders_;
    // Provider-only view retained internally for backend normalization;
    // consumers use pageStoreSources() so metadata is frame-frozen.
    std::vector<RasterOverlayTileProvider*> pageStoreProviders_;
    std::vector<RasterOverlayPageSource> pageStoreSources_;
    bool pageStoreHasDirectFallbackParity_ = true;
    std::shared_ptr<RasterAssetDepot> assetDepot_;

    friend class RasterOverlayRuntime;
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
/// The runtime owns ordering and activation policy.  Direct directComposite and
/// TerrainPageStore consume the same overlay view, while backend selection can
/// evolve behind this boundary without changing RasterOverlay configuration.
class RasterOverlayRuntime {
public:
    explicit RasterOverlayRuntime(
        std::vector<ActivatedRasterOverlay*> overlays = {});
    ~RasterOverlayRuntime();

    RasterOverlayRuntime(const RasterOverlayRuntime&) = delete;
    RasterOverlayRuntime& operator=(const RasterOverlayRuntime&) = delete;

    /// Runtime-owned configuration order. This is not a backend view and is
    /// therefore stable even when Direct or PageStore filters a slot.
    const std::vector<ActivatedRasterOverlay*>& configuredOverlays() const {
        return overlays_;
    }

    /// Freeze backend selection and canonical slot identity for one frame.
    /// Returns true when the Direct execution generation changed and existing
    /// per-tile mappings must be released before consuming the new snapshot.
    bool beginFrame(uint64_t frameNumber, RenderDevice* device);

    const RasterOverlayFrameContext& frameContext() const {
        return frameContext_;
    }

    /// Install a phase-one provider-view strategy for one consumer. Passing
    /// nullptr restores the default strategy. This does not yet replace the
    /// consumer's complete selection/request/upload/render lifecycle.
    bool setBackend(RasterOverlayBackendKind kind,
                    std::unique_ptr<RasterOverlayBackend> backend);

    const RasterOverlayBackend* backend(RasterOverlayBackendKind kind) const;

    /// Enable/disable one backend without changing overlay ownership. A
    /// disabled PageStore returns an empty provider view, which makes its
    /// consumer fail closed and preserve Direct directComposite fallback.
    void setBackendEnabled(RasterOverlayBackendKind backend, bool enabled);
    bool backendEnabled(RasterOverlayBackendKind backend) const;

private:
    const std::vector<RasterOverlayTileProvider*>& providersForBackend(
        RasterOverlayBackendKind backend,
        RenderDevice* device);

    std::vector<ActivatedRasterOverlay*> overlays_;
    std::shared_ptr<RasterAssetDepot> assetDepot_;
    std::unique_ptr<RasterOverlayBackend> directBackend_;
    std::unique_ptr<RasterOverlayBackend> pageStoreBackend_;
    std::vector<RasterOverlayTileProvider*> directProviders_;
    std::vector<RasterOverlayTileProvider*> pageStoreProviders_;
    RasterOverlayFrameContext frameContext_;
    uint64_t generation_ = 0;
    uint64_t directGeneration_ = 0;
    uint64_t pageStoreGeneration_ = 0;
    bool directFrameDirty_ = true;
    bool pageStoreFrameDirty_ = true;

};

} // namespace earth_engine
