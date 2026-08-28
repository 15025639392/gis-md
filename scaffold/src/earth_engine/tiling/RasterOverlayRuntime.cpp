#include "RasterOverlayRuntime.h"

#include "../layers/ActivatedRasterOverlay.h"
#include "../layers/RasterOverlay.h"
#include "../providers/ImageryProvider.h"
#include "../providers/RasterOverlayTileProvider.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace earth_engine {
namespace {

class ProviderStackRasterOverlayBackend final : public RasterOverlayBackend {
public:
    explicit ProviderStackRasterOverlayBackend(RasterOverlayBackendKind kind)
        : kind_(kind) {}

    RasterOverlayBackendKind kind() const override { return kind_; }

    const std::vector<RasterOverlayTileProvider*>& providers(
        const std::vector<ActivatedRasterOverlay*>& overlays,
        RenderDevice* device) override {
        providers_.clear();
        if (!enabled_) {
            return providers_;
        }
        providers_.reserve(overlays.size());
        for (ActivatedRasterOverlay* overlay : overlays) {
            if (!overlay) {
                continue;
            }
            if (RasterOverlayTileProvider* provider =
                    overlay->ensureTileProvider(device)) {
                providers_.push_back(provider);
            }
        }
        return providers_;
    }

    void setEnabled(bool enabled) override { enabled_ = enabled; }
    bool enabled() const override { return enabled_; }

private:
    RasterOverlayBackendKind kind_;
    bool enabled_ = true;
    std::vector<RasterOverlayTileProvider*> providers_;
};

std::unique_ptr<RasterOverlayBackend> makeDefaultBackend(
    RasterOverlayBackendKind kind) {
    return std::make_unique<ProviderStackRasterOverlayBackend>(kind);
}

bool samePresentationSnapshot(const RasterOverlayFrameSlot& lhs,
                              const RasterOverlayFrameSlot& rhs) {
    return lhs.runtimeSlot == rhs.runtimeSlot &&
           lhs.overlay == rhs.overlay &&
           lhs.directProvider == rhs.directProvider &&
           lhs.pageStoreProvider == rhs.pageStoreProvider &&
           lhs.visible == rhs.visible &&
           lhs.opacity == rhs.opacity &&
           lhs.role == rhs.role &&
           lhs.priority == rhs.priority &&
           lhs.fallbackPolicy == rhs.fallbackPolicy &&
           lhs.blocksCompleteRenderable == rhs.blocksCompleteRenderable &&
           lhs.projection == rhs.projection &&
           lhs.providerRevision == rhs.providerRevision;
}

bool samePresentationSnapshot(
    const std::vector<RasterOverlayFrameSlot>& lhs,
    const std::vector<RasterOverlayFrameSlot>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!samePresentationSnapshot(lhs[i], rhs[i])) {
            return false;
        }
    }
    return true;
}

bool samePageSource(const RasterOverlayPageSource& lhs,
                    const RasterOverlayPageSource& rhs) {
    return lhs.runtimeSlot == rhs.runtimeSlot &&
           lhs.provider == rhs.provider &&
           lhs.opacity == rhs.opacity &&
           lhs.role == rhs.role &&
           lhs.priority == rhs.priority &&
           lhs.fallbackPolicy == rhs.fallbackPolicy &&
           lhs.blocksCompleteRenderable == rhs.blocksCompleteRenderable &&
           lhs.projection == rhs.projection &&
           lhs.providerRevision == rhs.providerRevision;
}

bool samePageSources(const std::vector<RasterOverlayPageSource>& lhs,
                     const std::vector<RasterOverlayPageSource>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!samePageSource(lhs[i], rhs[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

RasterOverlayRuntime::RasterOverlayRuntime(
    std::vector<ActivatedRasterOverlay*> overlays)
    : overlays_(std::move(overlays))
    , assetDepot_(std::make_shared<RasterAssetDepot>())
    , directBackend_(makeDefaultBackend(RasterOverlayBackendKind::Direct))
    , pageStoreBackend_(
          makeDefaultBackend(RasterOverlayBackendKind::PageStore)) {
    frameContext_.assetDepot_ = assetDepot_;
    frameContext_.directOverlays_ = overlays_;
    frameContext_.slots_.resize(overlays_.size());
    for (size_t i = 0; i < overlays_.size(); ++i) {
        frameContext_.slots_[i].runtimeSlot = i;
        frameContext_.slots_[i].overlay = overlays_[i];
    }
}

RasterOverlayRuntime::~RasterOverlayRuntime() = default;

const std::vector<RasterOverlayTileProvider*>&
RasterOverlayRuntime::providersForBackend(
    RasterOverlayBackendKind backend,
    RenderDevice* device) {
    RasterOverlayBackend* selected = backend == RasterOverlayBackendKind::Direct
        ? directBackend_.get()
        : pageStoreBackend_.get();
    if (!selected) {
        // setBackend(nullptr) restores a default, so this is defensive only.
        static const std::vector<RasterOverlayTileProvider*> kNoProviders;
        return kNoProviders;
    }

    std::vector<RasterOverlayTileProvider*>& orderedProviders =
        backend == RasterOverlayBackendKind::Direct
        ? directProviders_
        : pageStoreProviders_;
    orderedProviders.clear();
    if (!selected->enabled()) {
        return orderedProviders;
    }

    const auto& selectedProviders = selected->providers(overlays_, device);
    std::unordered_set<RasterOverlayTileProvider*> selectedSet;
    selectedSet.reserve(selectedProviders.size());
    for (RasterOverlayTileProvider* provider : selectedProviders) {
        if (provider) {
            selectedSet.insert(provider);
        }
    }

    orderedProviders.reserve(overlays_.size());
    for (ActivatedRasterOverlay* overlay : overlays_) {
        if (!overlay) {
            continue;
        }
        // The backend owns the decision to instantiate providers. A filtered
        // custom backend must not cause excluded overlays to allocate a
        // provider merely because Runtime is normalizing its returned view.
        RasterOverlayTileProvider* provider = overlay->getTileProvider();
        if (!provider || selectedSet.count(provider) == 0) {
            continue;
        }
        provider->setAssetDepot(assetDepot_);
        orderedProviders.push_back(provider);
    }
    return orderedProviders;
}

bool RasterOverlayRuntime::beginFrame(
    uint64_t frameNumber,
    RenderDevice* device) {
    const std::vector<ActivatedRasterOverlay*> previousDirect =
        frameContext_.directOverlays_;
    const std::vector<RasterOverlayFrameSlot> previousSlots =
        frameContext_.slots_;
    const std::vector<RasterOverlayPageSource> previousPageStoreSources =
        frameContext_.pageStoreSources_;
    const bool previousPageStoreFallbackParity =
        frameContext_.pageStoreHasDirectFallbackParity_;
    const uint64_t previousDirectGeneration = directGeneration_;
    const bool directWasDirty = directFrameDirty_;
    const bool hasPublishedDirectGeneration = directGeneration_ != 0;

    if (directWasDirty && hasPublishedDirectGeneration) {
        for (ActivatedRasterOverlay* overlay : overlays_) {
            if (overlay) {
                overlay->invalidateDirectExecutionState();
            }
        }
    }

    const auto& direct = providersForBackend(
        RasterOverlayBackendKind::Direct, device);
    const auto& pageStore = providersForBackend(
        RasterOverlayBackendKind::PageStore, device);

    frameContext_.frameNumber_ = frameNumber;
    frameContext_.assetDepot_ = assetDepot_;
    frameContext_.slots_.clear();
    frameContext_.slots_.resize(overlays_.size());
    frameContext_.directOverlays_.assign(overlays_.size(), nullptr);
    frameContext_.directProviders_ = direct;
    frameContext_.pageStoreProviders_ = pageStore;
    frameContext_.pageStoreSources_.clear();
    frameContext_.pageStoreSources_.reserve(pageStore.size());

    std::unordered_set<RasterOverlayTileProvider*> directSet(
        direct.begin(), direct.end());
    std::unordered_set<RasterOverlayTileProvider*> pageStoreSet(
        pageStore.begin(), pageStore.end());
    std::vector<std::pair<size_t, RasterOverlayTileProvider*>>
        activeDirectSources;
    activeDirectSources.reserve(direct.size());
    for (size_t i = 0; i < overlays_.size(); ++i) {
        ActivatedRasterOverlay* overlay = overlays_[i];
        RasterOverlayTileProvider* provider =
            overlay ? overlay->getTileProvider() : nullptr;
        RasterOverlayFrameSlot& slot = frameContext_.slots_[i];
        slot.runtimeSlot = i;
        slot.overlay = overlay;
        if (overlay) {
            const RasterOverlay::Options& options =
                overlay->getOverlay().getOptions();
            slot.visible = options.visible;
            slot.opacity = std::clamp(options.opacity, 0.0f, 1.0f);
            slot.role = options.role;
            slot.priority = options.priority;
            slot.fallbackPolicy = options.fallbackPolicy;
            slot.blocksCompleteRenderable =
                options.blocksCompleteRenderable;
            overlay->publishFramePresentation(
                slot.visible,
                slot.opacity,
                slot.role,
                slot.priority,
                slot.fallbackPolicy,
                slot.blocksCompleteRenderable);
            slot.projection = overlay->getProjection();
            slot.providerRevision = provider
                ? provider->getImageryProvider().contentRevision()
                : 0;
        }
        if (provider && directSet.count(provider) != 0) {
            slot.directProvider = provider;
            frameContext_.directOverlays_[i] = overlay;
            if (slot.visible && slot.opacity > 0.0f) {
                activeDirectSources.emplace_back(slot.runtimeSlot, provider);
            }
        }
        if (provider && pageStoreSet.count(provider) != 0) {
            slot.pageStoreProvider = provider;
            if (slot.visible && slot.opacity > 0.0f) {
                frameContext_.pageStoreSources_.push_back(
                    RasterOverlayPageSource{
                        slot.runtimeSlot,
                        provider,
                        slot.opacity,
                        slot.role,
                        slot.priority,
                        slot.fallbackPolicy,
                        slot.blocksCompleteRenderable,
                        slot.projection,
                        slot.providerRevision});
            }
        }
    }

    frameContext_.pageStoreHasDirectFallbackParity_ =
        activeDirectSources.size() == frameContext_.pageStoreSources_.size();
    if (frameContext_.pageStoreHasDirectFallbackParity_) {
        for (size_t i = 0; i < activeDirectSources.size(); ++i) {
            const RasterOverlayPageSource& pageSource =
                frameContext_.pageStoreSources_[i];
            if (activeDirectSources[i].first != pageSource.runtimeSlot ||
                activeDirectSources[i].second != pageSource.provider) {
                frameContext_.pageStoreHasDirectFallbackParity_ = false;
                break;
            }
        }
    }

    const bool presentationChanged =
        !samePresentationSnapshot(previousSlots, frameContext_.slots_);
    const bool pageStoreSourcesChanged =
        !samePageSources(
            previousPageStoreSources, frameContext_.pageStoreSources_) ||
        previousPageStoreFallbackParity !=
            frameContext_.pageStoreHasDirectFallbackParity_;

    if (directFrameDirty_ ||
        previousDirect != frameContext_.directOverlays_) {
        if (!directWasDirty) {
            for (ActivatedRasterOverlay* overlay : overlays_) {
                if (overlay) {
                    overlay->invalidateDirectExecutionState();
                }
            }
        }
        ++directGeneration_;
        directFrameDirty_ = false;
    }
    if (pageStoreFrameDirty_ || pageStoreSourcesChanged) {
        ++pageStoreGeneration_;
        pageStoreFrameDirty_ = false;
    }
    if (frameContext_.directGeneration_ != directGeneration_ ||
        frameContext_.pageStoreGeneration_ != pageStoreGeneration_ ||
        presentationChanged) {
        ++generation_;
    }
    frameContext_.generation_ = generation_;
    frameContext_.directGeneration_ = directGeneration_;
    frameContext_.pageStoreGeneration_ = pageStoreGeneration_;
    for (RasterOverlayFrameSlot& slot : frameContext_.slots_) {
        slot.generation = generation_;
    }
    return previousDirectGeneration != directGeneration_;
}

bool RasterOverlayRuntime::setBackend(
    RasterOverlayBackendKind kind,
    std::unique_ptr<RasterOverlayBackend> backend) {
    if (backend && backend->kind() != kind) {
        return false;
    }
    if (!backend) {
        backend = makeDefaultBackend(kind);
    }
    switch (kind) {
        case RasterOverlayBackendKind::Direct:
            directBackend_ = std::move(backend);
            directFrameDirty_ = true;
            return true;
        case RasterOverlayBackendKind::PageStore:
            pageStoreBackend_ = std::move(backend);
            pageStoreFrameDirty_ = true;
            return true;
    }
    return false;
}

const RasterOverlayBackend* RasterOverlayRuntime::backend(
    RasterOverlayBackendKind kind) const {
    return kind == RasterOverlayBackendKind::Direct
        ? directBackend_.get()
        : pageStoreBackend_.get();
}

void RasterOverlayRuntime::setBackendEnabled(
    RasterOverlayBackendKind backend, bool enabled) {
    RasterOverlayBackend* selected = backend == RasterOverlayBackendKind::Direct
        ? directBackend_.get()
        : pageStoreBackend_.get();
    if (selected && selected->enabled() != enabled) {
        selected->setEnabled(enabled);
        if (backend == RasterOverlayBackendKind::Direct) {
            directFrameDirty_ = true;
        } else {
            pageStoreFrameDirty_ = true;
        }
    }
}

bool RasterOverlayRuntime::backendEnabled(
    RasterOverlayBackendKind backend) const {
    const RasterOverlayBackend* selected = this->backend(backend);
    return selected && selected->enabled();
}

} // namespace earth_engine
