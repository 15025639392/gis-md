#pragma once

#include "GltfModel.h"
#include "../tiling/TileID.h"

#include <array>
#include <memory>

namespace earth_engine {

// Child window inside the parent's texcoord space ([0,1] x [0,1]). Values are
// raw UV coordinates: for inverted-V texcoord sets the caller must flip the
// projected window into UV space before passing it here (v0 < v1 always).
struct GltfUpsampleUvWindow {
    double u0 = 0.0;
    double v0 = 0.0;
    double u1 = 1.0;
    double v1 = 1.0;
};

// Where the upsampled child sits inside each of the parent's texcoord spaces.
// texCoordSets is index-aligned with rasterOverlayDetails projections; sets at
// or beyond texCoordSetCount keep their parent values untouched. nativeUv
// covers SurfaceVertex::uv (linear geographic, south-up, never inverted).
struct GltfUpsampleWindows {
    std::array<GltfUpsampleUvWindow, kGltfMaxTexCoordSets> texCoordSets{};
    size_t texCoordSetCount = 0;
    GltfUpsampleUvWindow nativeUv{};
};

class GltfTerrainUpsampler {
public:
    // Clips the parent mesh to the child window of the textureCoordinateIndex
    // set and renormalizes every windowed texcoord set (plus the native uv) to
    // span [0,1] over the child. This matches cesium-native semantics, which
    // removes overlay texcoords during upsampling and regenerates them against
    // the child's own rectangle (RasterOverlayUtilities.cpp: "it will be
    // generated later"): because texcoords are linear in their projected
    // space, renormalizing the kept subrange is mathematically equivalent to
    // regenerating per vertex. Callers must pair the windows with child
    // rasterOverlayDetails rectangles derived by the same mapping so that the
    // engine invariant "UV [0,1] spans details.rasterOverlayRectangles[set]"
    // keeps holding for upsampled content.
    //
    // When windows is null, exact half-split quadrant windows are assumed
    // (TMS parity: odd x -> east, odd y -> north), which is correct for
    // linear-projection texcoords on TMS schemes only.
    static std::unique_ptr<GltfModel> upsampleForRasterOverlay(
        const GltfModel& parentModel,
        const UpsampledQuadtreeNode& childID,
        int textureCoordinateIndex = 0,
        bool hasInvertedVCoordinate = false,
        const GltfUpsampleWindows* windows = nullptr);

    // Exact half-split window for a quadrant child assuming TMS parity
    // (odd x -> east, odd y -> north). Used as fallback when no projected
    // rectangles are available to derive the real window.
    static GltfUpsampleUvWindow quadrantUvWindow(
        const UpsampledQuadtreeNode& childID,
        bool hasInvertedVCoordinate);
};

} // namespace earth_engine
