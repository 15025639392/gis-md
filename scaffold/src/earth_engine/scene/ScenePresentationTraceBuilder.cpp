#include "ScenePresentationTraceBuilder.h"

#include "Camera.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Transforms.h"
#include "../tiling/TilePlan.h"
#include "../tiling/Tileset.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>

namespace earth_engine {
namespace {

std::array<double, 3> toArray(const Vec3& v) {
    return {v.x(), v.y(), v.z()};
}

std::array<double, 16> toArray(const Mat4& m) {
    std::array<double, 16> values{};
    const double* raw = glm::value_ptr(m.raw());
    std::copy(raw, raw + values.size(), values.begin());
    return values;
}

double wrapPositiveRadians(double radians) {
    constexpr double kTwoPi = glm::two_pi<double>();
    double wrapped = std::fmod(radians, kTwoPi);
    if (wrapped < 0.0) {
        wrapped += kTwoPi;
    }
    return wrapped;
}

void populateCameraTrace(const FrameState& frameState,
                         PresentationTrace& trace) {
    trace.camera.frameId = frameState.frameId;
    trace.camera.viewportWidthPixels = frameState.viewportWidthPixels;
    trace.camera.viewportHeightPixels = frameState.viewportHeightPixels;
    trace.camera.devicePixelRatio = frameState.devicePixelRatio;

    if (!frameState.camera) {
        return;
    }

    const Camera& cam = *frameState.camera;
    const Vec3 target = cam.target();
    Cartographic targetCartographic =
        std::isnan(target.x())
            ? Ellipsoid::WGS84().cartesianToCartographic(cam.position())
            : Ellipsoid::WGS84().cartesianToCartographic(target);
    const Mat4 enuToEcef =
        Transforms::eastNorthUpToFixedFrame(
            Ellipsoid::WGS84().cartographicToCartesian(targetCartographic));
    const glm::dmat4 ecefToEnu = glm::inverse(enuToEcef.raw());
    const glm::dvec4 localDirection4 =
        ecefToEnu * glm::dvec4(cam.direction().raw(), 0.0);
    const glm::dvec3 localDirection =
        glm::normalize(glm::dvec3(localDirection4));

    trace.camera.verticalFovRadians = cam.verticalFovRadians();
    trace.camera.targetLongitudeDegrees =
        targetCartographic.longitudeDegrees();
    trace.camera.targetLatitudeDegrees =
        targetCartographic.latitudeDegrees();
    trace.camera.targetHeightMeters = targetCartographic.height();
    trace.camera.cameraHeightMeters = cam.getHeight();
    trace.camera.pitchRadians =
        std::asin(std::clamp(localDirection.z, -1.0, 1.0));
    trace.camera.headingRadians =
        wrapPositiveRadians(std::atan2(localDirection.x, localDirection.y));
    trace.camera.position = toArray(cam.position());
    trace.camera.direction = toArray(cam.direction());
    trace.camera.up = toArray(cam.up());
    trace.camera.right = toArray(cam.right());
}

void populateSelectorViewTrace(const FrameState& frameState,
                               PresentationTrace& trace) {
    trace.selectorViews.reserve(frameState.selectorViews.size());
    for (const SelectorView& view : frameState.selectorViews) {
        PresentationSelectorViewTrace viewTrace;
        viewTrace.position = toArray(view.position);
        viewTrace.direction = toArray(view.direction);
        viewTrace.viewportHeightPixels = view.viewportHeightPixels;
        viewTrace.projectionMatrix = toArray(view.projectionMatrix);
        trace.selectorViews.push_back(viewTrace);
    }
}

void appendTilesetTrace(PresentationTrace& trace, const Tileset* tileset) {
    if (!tileset) {
        return;
    }
    const TilePlan& plan = tileset->tilePlan();
    PresentationTilesetTrace tilesetTrace;
    tilesetTrace.visibleTiles = plan.visibleTiles;
    tilesetTrace.minVisibleZoom = plan.minVisibleZoom;
    tilesetTrace.maxVisibleZoom = plan.maxVisibleZoom;
    tilesetTrace.lodSizePixels = plan.lodSizePixels;
    tilesetTrace.renderEntries.reserve(plan.renderEntries.size());
    for (const TileRenderEntry& entry : plan.renderEntries) {
        PresentationRenderEntryTrace entryTrace;
        entryTrace.selectedKey = entry.selectedKey;
        entryTrace.renderKey = entry.renderKey;
        entryTrace.reason = entry.reason;
        entryTrace.opacity = entry.opacity;
        entryTrace.selectedThisFrame = entry.selectedThisFrame;
        entryTrace.usesAncestorFallback = entry.usesAncestorFallback;
        entryTrace.allowSynchronousMeshPrep =
            entry.allowSynchronousMeshPrep;
        entryTrace.surfaceClipEnabled = entry.surfaceClipEnabled;
        entryTrace.surfaceClipUv = entry.surfaceClipUv;
        tilesetTrace.renderEntries.push_back(entryTrace);
    }
    trace.tilesets.push_back(std::move(tilesetTrace));
}

void populateTilesetTrace(const ScenePresentationTraceInput& input,
                          PresentationTrace& trace) {
    appendTilesetTrace(trace, input.terrainTileset);
    for (const auto& tileset : input.additionalTilesets) {
        appendTilesetTrace(trace, tileset.get());
    }
}

void populateCommandTrace(const RenderCommandList& renderCommands,
                          PresentationTrace& trace) {
    trace.commands.reserve(renderCommands.size());
    for (const RenderCommand& command : renderCommands) {
        PresentationCommandTrace commandTrace;
        commandTrace.kind = command.kind;
        commandTrace.owner = command.owner;
        commandTrace.stableKey = command.stableKey;
        commandTrace.surfaceGeometryZoom = command.surfaceGeometryZoom;
        commandTrace.surfaceTextureZoom = command.surfaceTextureZoom;
        commandTrace.indexOffset = command.indexOffset;
        commandTrace.indexCount = command.indexCount;
        commandTrace.surfaceMeshIndexCount = command.surfaceMeshIndexCount;
        commandTrace.surfaceNoSkirtIndexCount =
            command.surfaceNoSkirtIndexCount;
        commandTrace.surfaceSkirtIndexCount =
            command.surfaceSkirtIndexCount;
        commandTrace.surfaceBaseRasterState =
            command.surfaceBaseRasterState;
        commandTrace.surfaceBaseIsRectangleTile =
            command.surfaceBaseIsRectangleTile;
        commandTrace.surfaceOverlayTextureCount =
            command.surfaceOverlayTextureCount;
        commandTrace.surfaceClipEnabled = command.surfaceClipEnabled;
        commandTrace.surfaceClipUv = command.surfaceClipUv;
        commandTrace.surfaceTransitionOpacity =
            command.surfaceTransitionOpacity;
        commandTrace.frameId = command.frameId;
        commandTrace.generation = command.generation;
        trace.commands.push_back(std::move(commandTrace));
    }
}

} // namespace

PresentationTrace ScenePresentationTraceBuilder::build(
    const ScenePresentationTraceInput& input) {
    PresentationTrace trace;
    populateCameraTrace(input.frameState, trace);
    populateSelectorViewTrace(input.frameState, trace);
    populateTilesetTrace(input, trace);
    populateCommandTrace(input.renderCommands, trace);
    return trace;
}

} // namespace earth_engine
