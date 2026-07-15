#pragma once

#include "../layers/ActivatedRasterOverlay.h"
#include "../layers/RasterOverlay.h"
#include "../providers/RasterOverlayTileProvider.h"

#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
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
        (void)rasterOverlays;
        return baseResourceRevision;
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
                               options.maximumSimultaneousTileLoads));
            mixDouble(signature, options.maximumScreenSpaceError);
            mix(signature, static_cast<uint64_t>(options.subTileCacheBytes));
            mix(signature, static_cast<uint64_t>(options.maximumTextureSize));
            mix(signature, static_cast<uint64_t>(options.minimumZoom));
            mix(signature, static_cast<uint64_t>(options.maximumZoom));
            mixDouble(signature, options.coverageRectangle.west());
            mixDouble(signature, options.coverageRectangle.south());
            mixDouble(signature, options.coverageRectangle.east());
            mixDouble(signature, options.coverageRectangle.north());
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

    static uint64_t mappingIdentity(
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
        uint64_t signature = kFnvOffset;
        mix(signature, static_cast<uint64_t>(rasterOverlays.size()));
        for (const auto* activeOverlay : rasterOverlays) {
            mix(signature, reinterpret_cast<uintptr_t>(activeOverlay));
        }
        return signature;
    }

    static uint64_t mappingRevision(
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
        uint64_t signature = kFnvOffset;
        mix(signature, static_cast<uint64_t>(rasterOverlays.size()));
        for (const auto* activeOverlay : rasterOverlays) {
            if (!activeOverlay) {
                mix(signature, 0);
                continue;
            }
            const RasterOverlayTileProvider* provider =
                activeOverlay->getTileProvider();
            mix(signature, provider ? provider->mappingRevision() : 0);
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

    static void mixDouble(uint64_t& signature, double value) {
        uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        mix(signature, bits);
    }
};

} // namespace earth_engine
