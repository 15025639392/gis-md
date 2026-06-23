#pragma once

#include "../renderer/RenderCommand.h"
#include "../tiling/TileKey.h"
#include "../tiling/TilePlan.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace earth_engine {

struct PresentationCameraTrace {
    uint64_t frameId = 0;
    int viewportWidthPixels = 0;
    int viewportHeightPixels = 0;
    float devicePixelRatio = 1.0f;
    double verticalFovRadians = 0.0;
    double targetLongitudeDegrees = 0.0;
    double targetLatitudeDegrees = 0.0;
    double targetHeightMeters = 0.0;
    double cameraHeightMeters = 0.0;
    double pitchRadians = 0.0;
    double headingRadians = 0.0;
    std::array<double, 3> position{0.0, 0.0, 0.0};
    std::array<double, 3> direction{0.0, 0.0, 0.0};
    std::array<double, 3> up{0.0, 0.0, 0.0};
    std::array<double, 3> right{0.0, 0.0, 0.0};
};

struct PresentationSelectorViewTrace {
    std::array<double, 3> position{0.0, 0.0, 0.0};
    std::array<double, 3> direction{0.0, 0.0, 0.0};
    int viewportHeightPixels = 0;
    std::array<double, 16> projectionMatrix{};
};

struct PresentationRenderEntryTrace {
    TileKey selectedKey;
    TileKey renderKey;
    TileRenderEntryReason reason = TileRenderEntryReason::Direct;
    float opacity = 1.0f;
    bool selectedThisFrame = true;
    bool usesAncestorFallback = false;
    bool allowSynchronousMeshPrep = true;
    bool surfaceClipEnabled = false;
    std::array<float, 4> surfaceClipUv{0.0f, 0.0f, 1.0f, 1.0f};
};

struct PresentationTilesetTrace {
    std::vector<TileKey> visibleTiles;
    std::vector<PresentationRenderEntryTrace> renderEntries;
    int minVisibleZoom = 0;
    int maxVisibleZoom = 0;
    double lodSizePixels = 0.0;
    int renderEntryAncestorFallbackCount = 0;
    int renderEntrySynchronousPrepCount = 0;
    int renderEntryDeferredPrepCount = 0;
    int renderEntryPlannedCommandCount = 0;
    int renderEntrySelectedPlannedCommandCount = 0;
    int renderEntryFadingPlannedCommandCount = 0;
    int renderEntryCommandDrawCount = 0;
    int renderEntrySelectedCommandDrawCount = 0;
    int renderEntryFadingCommandDrawCount = 0;
    int renderEntryCommandMissedDrawCount = 0;
    int renderEntrySelectedCommandMissedDrawCount = 0;
    int renderEntryFadingCommandMissedDrawCount = 0;
    int renderEntryCommandMissingSelectedCount = 0;
    int renderEntryCommandMissingRenderCount = 0;
    int renderEntryCommandDeferredCount = 0;
    int renderEntrySelectedCommandDeferredCount = 0;
    int renderEntryFadingCommandDeferredCount = 0;
};

struct PresentationCommandTrace {
    RenderCommandKind kind = RenderCommandKind::Unknown;
    std::string owner;
    std::string stableKey;
    int surfaceGeometryZoom = -1;
    int surfaceTextureZoom = -1;
    int indexOffset = 0;
    int indexCount = 0;
    int surfaceMeshIndexCount = 0;
    int surfaceNoSkirtIndexCount = 0;
    int surfaceSkirtIndexCount = 0;
    int surfaceBaseRasterState = 0;
    int surfaceBaseIsMappedRasterTile = 0;
    int surfaceOverlayTextureCount = 0;
    float surfaceClipEnabled = 0.0f;
    std::array<float, 4> surfaceClipUv{0.0f, 0.0f, 1.0f, 1.0f};
    float surfaceTransitionOpacity = 1.0f;
    uint64_t frameId = 0;
    uint64_t generation = 0;
};

struct PresentationTrace {
    PresentationCameraTrace camera;
    std::vector<PresentationSelectorViewTrace> selectorViews;
    std::vector<PresentationTilesetTrace> tilesets;
    std::vector<PresentationCommandTrace> commands;
};

} // namespace earth_engine
