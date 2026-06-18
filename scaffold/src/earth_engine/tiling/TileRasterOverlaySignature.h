#pragma once

#include "../layers/ActivatedRasterOverlay.h"
#include "../layers/RasterOverlay.h"
#include "../providers/RasterOverlayTileProvider.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace earth_engine {

class TileRasterOverlaySignature {
public:
    static uint64_t revision(
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
        uint64_t value = kFnvOffset;
        for (const auto* overlay : rasterOverlays) {
            value ^= overlay ? overlay->revision() : 0;
            value *= kFnvPrime;
        }
        return value;
    }

    static uint64_t selectionResourceRevision(
        uint64_t baseResourceRevision,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
        uint64_t combined = baseResourceRevision;
        combined ^= revision(rasterOverlays) + 0x9e3779b97f4a7c15ull +
                    (combined << 6) + (combined >> 2);
        return combined;
    }

    static uint64_t configuration(
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
        uint64_t signature = kFnvOffset;
        mix(signature, static_cast<uint64_t>(rasterOverlays.size()));
        for (const auto* activeOverlay : rasterOverlays) {
            if (!activeOverlay) {
                mix(signature, 0);
                continue;
            }

            const RasterOverlay& overlay = activeOverlay->getOverlay();
            const RasterOverlay::Options& options = overlay.getOptions();
            mix(signature, activeOverlay->visible() ? 1ull : 0ull);
            mix(signature, static_cast<uint64_t>(
                               options.blocksCompleteRenderable ? 1 : 0));
            mix(signature, static_cast<uint64_t>(options.role));
            mix(signature, static_cast<uint64_t>(options.priority));
            mix(signature, static_cast<uint64_t>(options.fallbackPolicy));
            mix(signature, static_cast<uint64_t>(std::lround(
                               static_cast<double>(activeOverlay->opacity()) *
                               1000000.0)));
            if (const RasterOverlayTileProvider* provider =
                    activeOverlay->getTileProvider()) {
                mix(signature, provider->isReady() ? 1ull : 0ull);
            } else {
                mix(signature, 0);
            }
        }
        return signature;
    }

    static bool hasPendingWork(
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
        for (const auto* overlay : rasterOverlays) {
            if (overlay && overlay->hasPendingWork()) {
                return true;
            }
        }
        return false;
    }

private:
    static constexpr uint64_t kFnvOffset = 1469598103934665603ull;
    static constexpr uint64_t kFnvPrime = 1099511628211ull;

    static void mix(uint64_t& signature, uint64_t value) {
        signature ^= value + 0x9e3779b97f4a7c15ull + (signature << 6) +
                     (signature >> 2);
    }
};

} // namespace earth_engine
