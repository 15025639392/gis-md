#include "SceneRenderPipeline.h"
#include "Camera.h"
#include "../debug/PerfTimer.h"
#include "../environment/AtmosphereBackgroundPass.h"
#include "../environment/SkyBox.h"
#include "../environment/SkyGradient.h"
#include "../layers/VectorLayer.h"
#include "../renderer/Renderer.h"
#include "../tiling/Tileset.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace earth_engine {
namespace {

void updateSurfaceCommandDiagnostics(const RenderCommandList& commands,
                                     uint64_t expectedFrameId,
                                     Diagnostics& diag) {
    diag.staleSurfaceCommands = 0;
    diag.missingGenerationSurfaceCommands = 0;
    diag.minSurfaceGeneration = 0;
    diag.maxSurfaceGeneration = 0;

    uint64_t minGeneration = std::numeric_limits<uint64_t>::max();
    uint64_t maxGeneration = 0;
    bool sawGeneration = false;

    for (const RenderCommand& cmd : commands) {
        if (cmd.kind != RenderCommandKind::SurfaceTile) continue;

        if (expectedFrameId != 0 && cmd.frameId != expectedFrameId) {
            ++diag.staleSurfaceCommands;
        }
        if (cmd.generation == 0) {
            ++diag.missingGenerationSurfaceCommands;
            continue;
        }

        minGeneration = std::min(minGeneration, cmd.generation);
        maxGeneration = std::max(maxGeneration, cmd.generation);
        sawGeneration = true;
    }

    if (sawGeneration) {
        diag.minSurfaceGeneration = minGeneration;
        diag.maxSurfaceGeneration = maxGeneration;
    }
}

} // namespace

void SceneRenderPipeline::render(Context context) {
    const double renderStartMs = perf::nowMs();
    context.commands.clear();
    reserveCommands(context);

    double skyMs = 0.0;
    double atmosphereMs = 0.0;
    double layerCommandsMs = 0.0;
    double fallbackGlobeMs = 0.0;
    double vectorCommandsMs = 0.0;
    double mvpUniformsMs = 0.0;
    double sortMs = 0.0;
    double surfaceDiagnosticsMs = 0.0;
    double validateMs = 0.0;
    double diagnosticsMs = 0.0;

    buildSkyCommands(context, skyMs);
    buildAtmosphereCommands(context, atmosphereMs);
    buildLayerCommands(
        context, layerCommandsMs, fallbackGlobeMs, vectorCommandsMs);
    applyMvpUniforms(context, mvpUniformsMs);
    sortAndValidate(context, sortMs, surfaceDiagnosticsMs, validateMs);
    aggregateDiagnostics(context, diagnosticsMs);

    context.frameState.diagnostics.renderCommandBuildMs =
        perf::nowMs() - renderStartMs;

    if (context.beforeSubmit) {
        context.beforeSubmit();
    }

    const double submitStartMs = perf::nowMs();
    context.renderer.submit(context.commands);
    context.frameState.diagnostics.renderSubmitMs =
        perf::nowMs() - submitStartMs;

    releaseRenderReferences(context);

    char buildDetail[384];
    std::snprintf(buildDetail, sizeof(buildDetail),
        "sky=%.2f atmo=%.2f layers=%.2f fallback=%.2f vector=%.2f mvp=%.2f sort=%.2f surfDiag=%.2f validate=%.2f diag=%.2f commands=%zu",
        skyMs,
        atmosphereMs,
        layerCommandsMs,
        fallbackGlobeMs,
        vectorCommandsMs,
        mvpUniformsMs,
        sortMs,
        surfaceDiagnosticsMs,
        validateMs,
        diagnosticsMs,
        context.commands.size());
    perf::logTiming(context.frameState.frameId,
                    "Scene.render.buildBreakdown",
                    context.frameState.diagnostics.renderCommandBuildMs,
                    buildDetail);

    char detail[192];
    std::snprintf(detail, sizeof(detail),
        "build=%.2f submit=%.2f draw=%d surface=%d mesh=%d",
        context.frameState.diagnostics.renderCommandBuildMs,
        context.frameState.diagnostics.renderSubmitMs,
        context.frameState.diagnostics.drawCalls,
        context.frameState.diagnostics.renderSurfaceTiles,
        context.frameState.diagnostics.surfaceMeshCount);
    perf::logTiming(context.frameState.frameId,
                    "Scene.render.total",
                    perf::nowMs() - renderStartMs,
                    detail);
}

void SceneRenderPipeline::reserveCommands(Context& context) const {
    size_t expectedCommands = 4 + context.vectorLayers.size() * 4;
    auto addExpectedTilesetCommands = [&](const Tileset* tileset) {
        if (!tileset) return;
        expectedCommands += tileset->tilePlan().renderEntries.size();
    };
    addExpectedTilesetCommands(context.terrainTileset);
    for (const auto& tileset : context.additionalTilesets) {
        addExpectedTilesetCommands(tileset.get());
    }
    if (context.commands.capacity() < expectedCommands) {
        context.commands.reserve(expectedCommands);
    }
}

void SceneRenderPipeline::buildSkyCommands(
    Context& context,
    double& skyMs) const {
    const double startMs = perf::nowMs();
    if (context.skyBox && context.skyBox->isReady() &&
        context.skyGradient && context.frameState.camera) {
        const auto& cam = *context.frameState.camera;
        Mat4 vm = cam.viewMatrix();
        const double* vmPtr = glm::value_ptr(vm.raw());
        float viewMatrix[16];
        for (int i = 0; i < 16; ++i) viewMatrix[i] = static_cast<float>(vmPtr[i]);

        const float vpW =
            static_cast<float>(context.frameState.viewportWidthPixels);
        const float vpH =
            static_cast<float>(context.frameState.viewportHeightPixels);
        Mat4 pm = cam.projectionMatrix(
            static_cast<double>(vpW), static_cast<double>(vpH));
        const double* pmPtr = glm::value_ptr(pm.raw());
        float projMatrix[16];
        for (int i = 0; i < 16; ++i) projMatrix[i] = static_cast<float>(pmPtr[i]);

        float nightFactor = static_cast<float>(
            context.skyGradient->sunElevation() < -0.05
                ? std::clamp(
                      std::exp(context.skyGradient->sunElevation() * 8.0),
                      0.0,
                      1.0)
                : 0.0);
        double spaceFactor =
            std::clamp((cam.getHeight() - 120000.0) / 780000.0, 0.0, 1.0);
        spaceFactor = spaceFactor * spaceFactor * (3.0 - 2.0 * spaceFactor);
        nightFactor = std::max(nightFactor, static_cast<float>(spaceFactor));

        context.commands.push_back(context.skyBox->buildCommand(
            viewMatrix, projMatrix, cam.isOrthographic(), nightFactor));
    }
    skyMs = perf::nowMs() - startMs;
}

void SceneRenderPipeline::buildAtmosphereCommands(
    Context& context,
    double& atmosphereMs) const {
    const double startMs = perf::nowMs();
    if (context.atmospherePass && context.atmospherePass->isReady() &&
        context.skyGradient && context.frameState.camera) {
        const auto& cam = *context.frameState.camera;
        const float vpW =
            static_cast<float>(context.frameState.viewportWidthPixels);
        const float vpH =
            static_cast<float>(context.frameState.viewportHeightPixels);
        Vec3 sunDir(context.frameState.lightDir.x,
                    context.frameState.lightDir.y,
                    context.frameState.lightDir.z);

        context.commands.push_back(context.atmospherePass->buildCommand(
            cam.position(),
            static_cast<float>(cam.verticalFovRadians()),
            static_cast<int>(vpW),
            static_cast<int>(vpH),
            cam.right(),
            cam.up(),
            cam.direction(),
            sunDir,
            context.skyGradient->parameters()));
    }
    atmosphereMs = perf::nowMs() - startMs;
}

void SceneRenderPipeline::buildLayerCommands(
    Context& context,
    double& layerCommandsMs,
    double& fallbackGlobeMs,
    double& vectorCommandsMs) const {
    const double layerStartMs = perf::nowMs();
    if (context.terrainTileset) {
        context.terrainTileset->buildRenderCommands(
            context.renderer, context.commands);
    }
    for (auto& tileset : context.additionalTilesets) {
        if (tileset) {
            tileset->buildRenderCommands(context.renderer, context.commands);
        }
    }
    layerCommandsMs = perf::nowMs() - layerStartMs;

    const double fallbackStartMs = perf::nowMs();
    const bool hasSurfaceTile =
        std::any_of(context.commands.begin(),
                    context.commands.end(),
                    [](const RenderCommand& cmd) {
                        return cmd.kind == RenderCommandKind::SurfaceTile;
                    });
    if (!hasSurfaceTile) {
        context.commands.insert(
            context.commands.begin(),
            context.renderer.makeGlobeCommand(context.frameState));
    }
    fallbackGlobeMs = perf::nowMs() - fallbackStartMs;

    const double vectorStartMs = perf::nowMs();
    for (auto& vLayer : context.vectorLayers) {
        if (vLayer->visible()) {
            vLayer->buildRenderCommands(
                context.frameState, context.renderer, context.commands);
        }
    }
    vectorCommandsMs = perf::nowMs() - vectorStartMs;
}

void SceneRenderPipeline::applyMvpUniforms(
    Context& context,
    double& mvpUniformsMs) const {
    const double startMs = perf::nowMs();
    if (context.frameState.camera) {
        const Camera& cam = *context.frameState.camera;
        const float vpW =
            static_cast<float>(context.frameState.viewportWidthPixels);
        const float vpH =
            static_cast<float>(context.frameState.viewportHeightPixels);

        const Mat4& viewRaw = cam.viewMatrix();
        const Mat4& projRaw = cam.projectionMatrix(
            static_cast<double>(vpW), static_cast<double>(vpH));
        glm::dmat4 viewD(viewRaw.raw());
        glm::dmat4 projD(projRaw.raw());
        glm::dmat4 viewProj = projD * viewD;

        for (auto& cmd : context.commands) {
            if (cmd.owner == "globe") continue;
            if (cmd.kind == RenderCommandKind::SurfaceTile &&
                cmd.hasSurfaceTileUniforms) {
                glm::dvec3 origin(cmd.surfaceTileOrigin[0],
                                  cmd.surfaceTileOrigin[1],
                                  cmd.surfaceTileOrigin[2]);
                glm::dmat4 model = glm::translate(glm::dmat4(1.0), origin);
                glm::dmat4 mvp = projD * viewD * model;
                glm::mat4 mvpFloat = glm::mat4(mvp);
                std::memcpy(cmd.surfaceModelViewProjection.data(),
                            glm::value_ptr(mvpFloat),
                            16 * sizeof(float));
                cmd.surfaceLightDir = {context.frameState.lightDir.x,
                                       context.frameState.lightDir.y,
                                       context.frameState.lightDir.z};
                continue;
            }
            if (cmd.kind == RenderCommandKind::GltfPrimitive ||
                cmd.kind == RenderCommandKind::GltfPrimitiveInstanced) {
                glm::dmat4 model(1.0);
                auto originIt = cmd.uniforms.find("u_modelOrigin");
                if (originIt != cmd.uniforms.end() &&
                    originIt->second.size() >= 3) {
                    glm::dvec3 origin(originIt->second[0],
                                      originIt->second[1],
                                      originIt->second[2]);
                    model = glm::translate(glm::dmat4(1.0), origin);
                }
                glm::dmat4 mvp = viewProj * model;
                glm::mat4 mvpFloat = glm::mat4(mvp);
                auto& mvpU = cmd.uniforms["u_modelViewProjection"];
                mvpU.resize(16);
                std::memcpy(
                    mvpU.data(), glm::value_ptr(mvpFloat), 16 * sizeof(float));
                cmd.uniforms["u_lightDir"] = {context.frameState.lightDir.x,
                                              context.frameState.lightDir.y,
                                              context.frameState.lightDir.z};
                if (cmd.hasWorldSortCenter) {
                    const Vec3 center(cmd.worldSortCenter[0],
                                      cmd.worldSortCenter[1],
                                      cmd.worldSortCenter[2]);
                    cmd.translucentSortDepth =
                        (center - cam.position()).dot(cam.direction());
                    cmd.hasTranslucentSortDepth = true;
                }
                continue;
            }
            auto& mvpU = cmd.uniforms["u_modelViewProjection"];
            if (mvpU.empty()) {
                mvpU.resize(16);
                std::memcpy(
                    mvpU.data(), glm::value_ptr(viewProj), 16 * sizeof(float));
            }
            if (cmd.owner == "surface_tile") {
                cmd.uniforms["u_lightDir"] = {context.frameState.lightDir.x,
                                              context.frameState.lightDir.y,
                                              context.frameState.lightDir.z};
            }
        }
    }
    mvpUniformsMs = perf::nowMs() - startMs;
}

void SceneRenderPipeline::sortAndValidate(
    Context& context,
    double& sortMs,
    double& surfaceDiagnosticsMs,
    double& validateMs) const {
    const double sortStartMs = perf::nowMs();
    bool needsSort = false;
    bool hasTranslucentGltf = false;
    for (size_t i = 1; i < context.commands.size(); ++i) {
        if (context.commands[i - 1].blend &&
            (context.commands[i - 1].kind == RenderCommandKind::GltfPrimitive ||
             context.commands[i - 1].kind ==
                 RenderCommandKind::GltfPrimitiveInstanced)) {
            hasTranslucentGltf = true;
        }
        if (mvpRenderOrder(context.commands[i - 1].kind) >
            mvpRenderOrder(context.commands[i].kind)) {
            needsSort = true;
            break;
        }
    }
    if (!context.commands.empty()) {
        const RenderCommand& last = context.commands.back();
        if (last.blend &&
            (last.kind == RenderCommandKind::GltfPrimitive ||
             last.kind == RenderCommandKind::GltfPrimitiveInstanced)) {
            hasTranslucentGltf = true;
        }
    }
    if (needsSort || hasTranslucentGltf) {
        sortMvpRenderCommands(context.commands);
    }
    sortMs = perf::nowMs() - sortStartMs;

    const double surfaceStartMs = perf::nowMs();
    updateSurfaceCommandDiagnostics(
        context.commands,
        context.frameState.frameId,
        context.frameState.diagnostics);
    surfaceDiagnosticsMs = perf::nowMs() - surfaceStartMs;

    const double validateStartMs = perf::nowMs();
    if (auto error = validateMvpRenderCommands(
            context.commands, context.frameState.frameId)) {
        throw std::runtime_error(
            "MVP render command validation failed for '" + error->owner +
            "': " + error->message);
    }
    validateMs = perf::nowMs() - validateStartMs;
}

void SceneRenderPipeline::aggregateDiagnostics(
    Context& context,
    double& diagnosticsMs) const {
    const double startMs = perf::nowMs();
    auto& diag = context.frameState.diagnostics;
    diag.drawCalls = static_cast<int>(context.commands.size());
    diag.visibleTiles = 0;
    diag.contentTilesets = static_cast<int>(context.additionalTilesets.size());
    diag.contentVisibleTiles = 0;
    diag.cachedTextures = 0;
    diag.queuedRequests = 0;
    diag.loadingRequests = 0;
    diag.loadQueuePreloadRequests = 0;
    diag.loadQueueNormalRequests = 0;
    diag.loadQueueUrgentRequests = 0;
    diag.pendingTerrainRequests = 0;
    diag.pendingTerrainUploads = 0;
    diag.pendingTerrainTerminalResults = 0;
    diag.pendingContentRequests = 0;
    diag.pendingContentUploads = 0;
    diag.pendingContentTerminalResults = 0;
    diag.gpuTextureCount = 0;
    diag.renderSurfaceTiles = 0;
    diag.renderGltfPrimitives = 0;
    diag.surfaceMeshCount = 0;
    diag.imageryAttachments = 0;
    diag.imageryExactAttachments = 0;
    diag.imageryParentFallbackAttachments = 0;
    diag.imageryMissingTiles = 0;
    diag.imageryUnsupportedTiles = 0;
    diag.imageryTransitionTiles = 0;
    diag.imageryKickedTiles = 0;
    diag.imageryAncestorRetainedTiles = 0;
    diag.imageryMinTargetZoom = 0;
    diag.imageryMaxTargetZoom = 0;
    diag.imageryMinTextureZoom = 0;
    diag.imageryMaxTextureZoom = 0;
    diag.lodSizePixels = 0.0;
    diag.minVisibleZoom = 0;
    diag.maxVisibleZoom = 0;
    diag.quadtreeEqualZoomLayers = 0;
    diag.quadtreeFadingNodes = 0;
    diag.quadtreeNeighborLinks = 0;
    diag.quadtreeNeighborBalancedTiles = 0;
    diag.quadtreeRenderingNodes = 0;
    diag.quadtreeWalkthroughNodes = 0;
    diag.quadtreeNotRenderingNodes = 0;
    diag.quadtreeSelectionRenderedNodes = 0;
    diag.quadtreeSelectionRefinedNodes = 0;
    diag.quadtreeSelectionKickedNodes = 0;
    diag.quadtreeSelectionOccludedNodes = 0;
    diag.quadtreeSelectionWaitingForOcclusionResultsNodes = 0;
    diag.quadtreeCulledTilesVisited = 0;
    diag.quadtreeSelectionAncestorMeetsSseNodes = 0;
    diag.quadtreeCameraInsideNodes = 0;
    diag.quadtreeInFrustumNodes = 0;
    diag.quadtreeHorizonTangentPreservedNodes = 0;
    diag.quadtreeEqualZoomSecondPassNodes = 0;
    diag.mercatorTileCount = 0;
    diag.northPolarTileCount = 0;
    diag.southPolarTileCount = 0;
    diag.surfaceMeshBytes = 0;
    diag.terrainCachedTiles = 0;
    diag.terrainLoadUnloadingTiles = 0;
    diag.terrainLoadFailedTemporarilyTiles = 0;
    diag.terrainLoadUnloadedTiles = 0;
    diag.terrainLoadContentLoadingTiles = 0;
    diag.terrainLoadContentLoadedTiles = 0;
    diag.terrainLoadDoneTiles = 0;
    diag.terrainLoadFailedTiles = 0;
    diag.terrainContentUnknownTiles = 0;
    diag.terrainContentEmptyTiles = 0;
    diag.terrainContentExternalTiles = 0;
    diag.terrainContentRenderTiles = 0;
    diag.terrainUnloadQueueTiles = 0;
    diag.missingRasterOverlayProjections = 0;
    diag.terrainGeneration = 0;
    diag.terrainSurfaceMeshes = 0;
    diag.terrainParentFallbackMeshes = 0;
    diag.terrainReadySurfaceMeshes = 0;
    diag.terrainTransitionSurfaceMeshes = 0;
    diag.ellipsoidSurfaceMeshes = 0;

    std::unordered_set<const Texture*> surfaceTextures;
    bool sawSurfaceGeometryZoom = false;
    bool sawSurfaceTextureZoom = false;
    for (const RenderCommand& cmd : context.commands) {
        if (cmd.kind == RenderCommandKind::SurfaceTile) {
            ++diag.renderSurfaceTiles;
            ++diag.surfaceMeshCount;
            ++diag.terrainSurfaceMeshes;
            ++diag.terrainReadySurfaceMeshes;
            if (!cmd.textures.empty()) {
                ++diag.imageryExactAttachments;
                for (const Texture* texture : cmd.textures) {
                    if (texture) {
                        surfaceTextures.insert(texture);
                    }
                }
                if (cmd.surfaceGeometryZoom >= 0) {
                    if (!sawSurfaceGeometryZoom) {
                        diag.imageryMinTargetZoom = cmd.surfaceGeometryZoom;
                        diag.imageryMaxTargetZoom = cmd.surfaceGeometryZoom;
                        sawSurfaceGeometryZoom = true;
                    } else {
                        diag.imageryMinTargetZoom =
                            std::min(diag.imageryMinTargetZoom,
                                     cmd.surfaceGeometryZoom);
                        diag.imageryMaxTargetZoom =
                            std::max(diag.imageryMaxTargetZoom,
                                     cmd.surfaceGeometryZoom);
                    }
                }
                if (cmd.surfaceTextureZoom >= 0) {
                    if (!sawSurfaceTextureZoom) {
                        diag.imageryMinTextureZoom = cmd.surfaceTextureZoom;
                        diag.imageryMaxTextureZoom = cmd.surfaceTextureZoom;
                        sawSurfaceTextureZoom = true;
                    } else {
                        diag.imageryMinTextureZoom =
                            std::min(diag.imageryMinTextureZoom,
                                     cmd.surfaceTextureZoom);
                        diag.imageryMaxTextureZoom =
                            std::max(diag.imageryMaxTextureZoom,
                                     cmd.surfaceTextureZoom);
                    }
                }
            } else {
                ++diag.imageryMissingTiles;
            }
        } else if (cmd.kind == RenderCommandKind::GltfPrimitive ||
                   cmd.kind == RenderCommandKind::GltfPrimitiveInstanced) {
            ++diag.renderGltfPrimitives;
        }
    }

    auto addTilesetDiagnostics = [&](Tileset& tileset, bool terrain) {
        const TilePlan& plan = tileset.tilePlan();
        const TilesetLoadDiagnostics loadDiag = tileset.loadDiagnostics();
        if (terrain) {
            diag.visibleTiles = static_cast<int>(plan.visibleTiles.size());
            diag.terrainCachedTiles = tileset.cachedTerrainTiles();
            diag.pendingTerrainRequests = loadDiag.pendingTerrainRequests;
            diag.pendingTerrainUploads = loadDiag.pendingTerrainUploads;
            diag.pendingTerrainTerminalResults =
                loadDiag.pendingTerrainTerminalResults;
            diag.minVisibleZoom = plan.minVisibleZoom;
            diag.maxVisibleZoom = plan.maxVisibleZoom;
            diag.lodSizePixels = plan.lodSizePixels;
            diag.quadtreeRenderingNodes = plan.renderingNodeCount;
            diag.quadtreeWalkthroughNodes = plan.walkthroughNodeCount;
            diag.quadtreeNotRenderingNodes = plan.notRenderingNodeCount;
            diag.quadtreeSelectionRenderedNodes = plan.selectionRenderedCount;
            diag.quadtreeSelectionRefinedNodes = plan.selectionRefinedCount;
            diag.quadtreeSelectionKickedNodes = plan.selectionKickedCount;
            diag.quadtreeSelectionOccludedNodes = plan.selectionOccludedCount;
            diag.quadtreeSelectionWaitingForOcclusionResultsNodes =
                plan.selectionWaitingForOcclusionResultsCount;
            diag.quadtreeCulledTilesVisited = plan.culledTilesVisitedCount;
            diag.quadtreeSelectionAncestorMeetsSseNodes =
                plan.selectionAncestorMeetsSseCount;
            diag.quadtreeFadingNodes = plan.fadingNodeCount;
            diag.quadtreeCameraInsideNodes = plan.cameraInsideNodeCount;
            diag.quadtreeInFrustumNodes = plan.inFrustumNodeCount;
            diag.mercatorTileCount = plan.mercatorTileCount;
            diag.northPolarTileCount = plan.northPolarTileCount;
            diag.southPolarTileCount = plan.southPolarTileCount;
        } else {
            diag.contentVisibleTiles +=
                static_cast<int>(plan.visibleTiles.size());
        }

        diag.queuedRequests += loadDiag.loadQueueTotal();
        diag.loadingRequests +=
            loadDiag.pendingTerrainTotal() + loadDiag.pendingContentTotal();
        diag.loadQueuePreloadRequests += loadDiag.loadQueuePreloadRequests;
        diag.loadQueueNormalRequests += loadDiag.loadQueueNormalRequests;
        diag.loadQueueUrgentRequests += loadDiag.loadQueueUrgentRequests;
        diag.pendingContentRequests += loadDiag.pendingContentRequests;
        diag.pendingContentUploads += loadDiag.pendingContentUploads;
        diag.pendingContentTerminalResults +=
            loadDiag.pendingContentTerminalResults;
        diag.surfaceMeshBytes += static_cast<int>(tileset.totalBytesUsed());
        diag.terrainContentUnknownTiles += loadDiag.contentUnknownTiles;
        diag.terrainContentEmptyTiles += loadDiag.contentEmptyTiles;
        diag.terrainContentExternalTiles += loadDiag.contentExternalTiles;
        diag.terrainContentRenderTiles += loadDiag.contentRenderTiles;
        if (terrain) {
            diag.terrainLoadUnloadingTiles = loadDiag.loadUnloadingTiles;
            diag.terrainLoadFailedTemporarilyTiles =
                loadDiag.loadFailedTemporarilyTiles;
            diag.terrainLoadUnloadedTiles = loadDiag.loadUnloadedTiles;
            diag.terrainLoadContentLoadingTiles =
                loadDiag.loadContentLoadingTiles;
            diag.terrainLoadContentLoadedTiles = loadDiag.loadContentLoadedTiles;
            diag.terrainLoadDoneTiles = loadDiag.loadDoneTiles;
            diag.terrainLoadFailedTiles = loadDiag.loadFailedTiles;
            diag.terrainUnloadQueueTiles = loadDiag.unloadQueueTiles;
            diag.missingRasterOverlayProjections =
                loadDiag.missingRasterOverlayProjections;
        }
    };

    if (context.terrainTileset) {
        addTilesetDiagnostics(*context.terrainTileset, true);
    }
    for (const auto& tileset : context.additionalTilesets) {
        if (tileset) {
            addTilesetDiagnostics(*tileset, false);
        }
    }

    diag.gpuTextureCount = static_cast<int>(surfaceTextures.size());
    if (diag.gpuTextureCount == 0) {
        diag.gpuTextureCount = diag.cachedTextures;
    }
    diag.imageryAttachments =
        diag.imageryExactAttachments + diag.imageryParentFallbackAttachments;
    diagnosticsMs = perf::nowMs() - startMs;
}

void SceneRenderPipeline::releaseRenderReferences(Context& context) const {
    if (context.terrainTileset) {
        context.terrainTileset->releaseRenderReferences();
    }
    for (auto& tileset : context.additionalTilesets) {
        if (tileset) {
            tileset->releaseRenderReferences();
        }
    }
}

} // namespace earth_engine
