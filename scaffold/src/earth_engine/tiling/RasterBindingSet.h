#pragma once

#include "RasterResolution.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class RasterOverlayFrameContext;
struct TilesetTile;

/// One render-ready binding resolved from a stable Runtime overlay slot.
/// Packing into GPU texture units happens later; runtimeSlot remains the
/// authoritative identity shared with mapping and projection details.
struct RasterBinding {
    size_t runtimeSlot = 0;
    RasterResolution resolution;
    DirectRasterSampleDescriptor directSample;
};

/// Immutable per-tile snapshot used by readiness diagnostics and draw command
/// construction. It prevents render code from independently re-walking the
/// mutable overlay stack and re-deriving a different slot/binding decision.
class RasterBindingSet {
public:
    static RasterBindingSet resolve(
        const TilesetTile& tile,
        const RasterOverlayFrameContext& frame);

    const std::vector<RasterBinding>& bindings() const { return bindings_; }
    bool empty() const { return bindings_.empty(); }
    size_t size() const { return bindings_.size(); }
    const RasterBinding* bindingAtRuntimeSlot(size_t slot) const {
        return slot < bindings_.size() ? &bindings_[slot] : nullptr;
    }

private:
    std::vector<RasterBinding> bindings_;
};

} // namespace earth_engine
