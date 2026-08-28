#pragma once

#include "earth_engine/tiling/RasterOverlayRuntime.h"

namespace earth_engine::testing {

/// Published empty overlay frame for geometry-only unit tests. This keeps the
/// production contract strict: every consumer receives a real snapshot from
/// RasterOverlayRuntime::beginFrame(), even when the configured stack is empty.
inline const RasterOverlayFrameContext& emptyRasterOverlayFrame() {
    struct Holder {
        RasterOverlayRuntime runtime;

        Holder() {
            runtime.beginFrame(0, nullptr);
        }
    };

    static const Holder holder;
    return holder.runtime.frameContext();
}

} // namespace earth_engine::testing
