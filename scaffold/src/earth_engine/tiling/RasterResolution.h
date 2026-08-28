#pragma once

#include "../layers/RasterOverlay.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace earth_engine {

class DirectRasterMapping;
class Texture;

/// Backend-neutral result of resolving one overlay slot for one terrain tile.
///
/// Direct and PageStore are allowed to produce this same vocabulary.  The
/// current Direct adapter fills it from DirectRasterMapping; PageStore
/// can later publish an equivalent result without leaking Direct executor
/// state to command finalization.
enum class RasterRequestState {
    NoMapping,
    Placeholder,
    Unloaded,
    Loading,
    Loaded,
    Failed,
    Empty,
};

enum class RasterContentForm {
    None,
    Exact,
    Composed,
};

enum class RasterSourceRelation {
    None,
    Own,
    Ancestor,
};

enum class RasterAttachmentState {
    Unattached,
    TemporarilyAttached,
    Attached,
};

struct RasterResolution {
    RasterRequestState requestState = RasterRequestState::NoMapping;
    RasterContentForm contentForm = RasterContentForm::None;
    RasterSourceRelation sourceRelation = RasterSourceRelation::None;
    RasterAttachmentState attachment = RasterAttachmentState::Unattached;
    bool hasCoverage = false;
    bool coverReady = false;
    bool drawable = false;
    bool targetPending = false;
    bool targetFailed = false;
    bool allowedByPolicy = false;
    bool visible = true;
    int desiredZoom = -1;
    int resolvedZoom = -1;
    float opacity = 1.0f;
    RasterOverlayRole role = RasterOverlayRole::BaseImagery;
    RasterOverlayPriority priority = RasterOverlayPriority::High;
    RasterOverlayFallbackPolicy fallbackPolicy =
        RasterOverlayFallbackPolicy::AncestorOrPlaceholder;
    bool blocksCompleteRenderable = true;
    uint64_t generation = 0;
};

/// Backend-neutral presentation state for an ordered raster stack.
///
/// Direct resolves one sample per Runtime slot, while PageStore resolves the
/// complete ordered stack as one array+indirection attachment. This aggregate
/// vocabulary lets arbitration, diagnostics and differential tests compare
/// presentation without pretending both backends share one texture shape.
enum class RasterStackCoverage {
    None,
    Partial,
    Full,
};

struct RasterStackResolution {
    bool compatible = false;
    bool attached = false;
    bool drawable = false;
    bool fallbackRequired = true;
    bool fullyResident = false;
    RasterStackCoverage coverage = RasterStackCoverage::None;
    size_t sourceCount = 0;
    uint64_t generation = 0;
};

/// Direct backend sample descriptor. PageStore intentionally has a different
/// descriptor (array + indirection + placement); keeping this outside
/// RasterResolution prevents the semantic contract from pretending every
/// backend is one Texture + one affine UV.
struct DirectRasterSampleDescriptor {
    Texture* texture = nullptr;
    int32_t textureCoordinateId = -1;
    float offsetU = 0.0f;
    float offsetV = 0.0f;
    float scaleU = 1.0f;
    float scaleV = 1.0f;
    std::shared_ptr<const void> resourceLease;
};

struct DirectRasterBindingResolution {
    RasterResolution resolution;
    DirectRasterSampleDescriptor sample;
};

/// Adapt the current Direct mapping state to the neutral resolution
/// vocabulary. No request, cache, or GPU work is performed.
RasterResolution resolveDirectRasterResolution(
    const DirectRasterMapping* mapped);
DirectRasterBindingResolution resolveDirectRasterBinding(
    const DirectRasterMapping* mapped);

/// Apply the backend-neutral presentation/fallback policy to a resolved
/// sample. This deliberately depends only on RasterResolution so command and
/// readiness consumers never need the Direct mapping object.
bool rasterResolutionAllowedByPolicy(const RasterResolution& resolution);

} // namespace earth_engine
