#include "Scene.h"
#include "Camera.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Cartographic.h"
#include "../debug/PerfTimer.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <limits>
#include <utility>
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

Scene::Scene()
    : camera_(std::make_unique<Camera>()),
      cameraController_(std::make_unique<CameraController>(camera_.get())),
      inputManager_(std::make_unique<InputManager>()),
      pickingService_(std::make_unique<PickingService>()),
      selectionManager_(std::make_unique<SelectionManager>()),
      timeController_(std::make_unique<TimeController>()),
      skyGradient_(std::make_unique<SkyGradient>()),
      atmospherePass_(std::make_unique<AtmosphereBackgroundPass>()),
      skyBox_(std::make_unique<SkyBox>()) {

    // OpenGlobus PlanetCamera reverse-Z defaults: near=150, far=1e12.
    camera_->setPerspective(
        camera_->verticalFovRadians(),
        150.0,
        1e12);
    // Depth func/clear are configured per-platform by RenderDevice.
    globeMesh_ = Globe::createMesh(96, 48);
    configureCameraSurfacePicker();
    setupSelectionCallbacks();
    setupInputCallback();
}

Scene::~Scene() {
    renderer_.reset();
}

bool Scene::setRenderDevice(RenderDevice* device) {
    renderDevice_ = device;
    if (!device) {
        renderer_.reset();
        return false;
    }

    renderer_ = std::make_unique<Renderer>(device);
    if (!renderer_->initialize(globeMesh_)) {
        fprintf(stderr, "[Scene] renderer_->initialize() FAILED\n");
        return false;
    }

    // 初始化环境系统渲染 Pass
    atmospherePass_->initialize(device);
    skyBox_->initialize(device);

    return true;
}

void Scene::setViewport(int widthPixels, int heightPixels, float dpr) {
    frameState_.viewportWidthPixels = widthPixels;
    frameState_.viewportHeightPixels = heightPixels;
    frameState_.devicePixelRatio = dpr;

    if (cameraController_) {
        cameraController_->setViewport(widthPixels, heightPixels);
    }
}

void Scene::update(double deltaSeconds) {
    const double updateStartMs = perf::nowMs();

    frameState_.diagnostics.cameraUpdateMs = 0.0;
    frameState_.diagnostics.environmentUpdateMs = 0.0;
    frameState_.diagnostics.basemapStackUpdateMs = 0.0;
    frameState_.diagnostics.terrainUpdateMs = 0.0;
    frameState_.diagnostics.contentTilesetUpdateMs = 0.0;
    frameState_.diagnostics.renderCommandBuildMs = 0.0;
    frameState_.diagnostics.renderSubmitMs = 0.0;

    if (cameraController_) {
        const double startMs = perf::nowMs();
        cameraController_->update(deltaSeconds);
        frameState_.diagnostics.cameraUpdateMs = perf::nowMs() - startMs;
    }

    elapsedTime_ += deltaSeconds;
    frameState_.frameId = ++frameId_;
    frameState_.timeSeconds = elapsedTime_;
    frameState_.deltaSeconds = deltaSeconds;
    frameState_.camera = camera_.get();
    populateSelectorViews();
    constexpr double kInteractionFocusTtlSeconds = 2.5;
    frameState_.hasInteractionFocus =
        hasInteractionFocus_ &&
        interactionFocusTimeSeconds_ >= 0.0 &&
        elapsedTime_ - interactionFocusTimeSeconds_ <= kInteractionFocusTtlSeconds;
    frameState_.interactionFocusDirection = frameState_.hasInteractionFocus
        ? interactionFocusDirection_
        : Vec3::zero();

    // 更新 FPS（5 帧平滑）
    if (deltaSeconds > 0.0) {
        frameState_.diagnostics.frameTimeMs = deltaSeconds * 1000.0;
        constexpr double kFpsSmoothing = 0.1;
        frameState_.diagnostics.fps =
            frameState_.diagnostics.fps * (1.0 - kFpsSmoothing) +
            (1.0 / deltaSeconds) * kFpsSmoothing;
    }

    {
        const double startMs = perf::nowMs();
        Vec3 sunDir = SunDirection::compute(timeController_->julianDate());
        double camAlt = camera_->getHeight();
        Vec3 localUp = Ellipsoid::WGS84().geodeticSurfaceNormal(camera_->position());
        skyGradient_->update(sunDir, localUp, camAlt);
        frameState_.lightDir = {
            static_cast<float>(sunDir.x()),
            static_cast<float>(sunDir.y()),
            static_cast<float>(sunDir.z())
        };
        auto& hc = skyGradient_->horizonColor();
        frameState_.clearR = hc[0];
        frameState_.clearG = hc[1];
        frameState_.clearB = hc[2];
        frameState_.diagnostics.environmentUpdateMs = perf::nowMs() - startMs;
    }

    // 统一 Tileset 更新（cesium-native 对齐）
    if (tileset_) {
        const double startMs = perf::nowMs();
        tileset_->update(frameState_);
        frameState_.diagnostics.terrainUpdateMs = perf::nowMs() - startMs;
    }
    if (!additionalTilesets_.empty()) {
        const double startMs = perf::nowMs();
        for (auto& tileset : additionalTilesets_) {
            if (tileset) {
                tileset->update(frameState_);
            }
        }
        frameState_.diagnostics.contentTilesetUpdateMs =
            perf::nowMs() - startMs;
    }

    char detail[192];
    std::snprintf(detail, sizeof(detail),
        "camera=%.2f env=%.2f basemap=%.2f terrain=%.2f content=%.2f",
        frameState_.diagnostics.cameraUpdateMs,
        frameState_.diagnostics.environmentUpdateMs,
        frameState_.diagnostics.basemapStackUpdateMs,
        frameState_.diagnostics.terrainUpdateMs,
        frameState_.diagnostics.contentTilesetUpdateMs);
    perf::logTiming(frameState_.frameId,
                    "Scene.update.total",
                    perf::nowMs() - updateStartMs,
                    detail);
}

void Scene::setSelectorViewOverride(
    std::vector<FrameState::SelectorView> selectorViews) {
    hasSelectorViewOverride_ = true;
    selectorViewOverride_ = std::move(selectorViews);
}

void Scene::clearSelectorViewOverride() {
    hasSelectorViewOverride_ = false;
    selectorViewOverride_.clear();
}

void Scene::setOcclusionCallback(Tileset::OcclusionCallback callback) {
    occlusionCallback_ = std::move(callback);
    if (tileset_) {
        tileset_->setOcclusionCallback(occlusionCallback_);
    }
    for (auto& tileset : additionalTilesets_) {
        if (tileset) {
            tileset->setOcclusionCallback(occlusionCallback_);
        }
    }
}

void Scene::clearOcclusionCallback() {
    occlusionCallback_ = nullptr;
    if (tileset_) {
        tileset_->clearOcclusionCallback();
    }
    for (auto& tileset : additionalTilesets_) {
        if (tileset) {
            tileset->clearOcclusionCallback();
        }
    }
}

void Scene::populateSelectorViews() {
    frameState_.selectorViews.clear();
    if (hasSelectorViewOverride_) {
        frameState_.selectorViews = selectorViewOverride_;
        return;
    }

    if (!frameState_.camera) {
        return;
    }

    FrameState::SelectorView selectorView;
    selectorView.position = frameState_.camera->position();
    selectorView.direction = frameState_.camera->direction();
    selectorView.frustum = frameState_.camera->frustum(
        static_cast<double>(frameState_.viewportWidthPixels),
        static_cast<double>(frameState_.viewportHeightPixels));
    selectorView.verticalFovRadians = frameState_.camera->verticalFovRadians();
    selectorView.viewportHeightPixels = frameState_.viewportHeightPixels;
    frameState_.selectorViews.push_back(selectorView);
}

void Scene::render() {
    if (!renderer_ || !isReady()) return;

    const double renderStartMs = perf::nowMs();
    RenderCommandList& commands = renderCommands_;
    commands.clear();
    size_t expectedCommands = 4 + vectorLayers_.size() * 4;
    auto addExpectedTilesetCommands = [&](const Tileset* tileset) {
        if (!tileset) return;
        expectedCommands += tileset->tilePlan().visibleTiles.size();
        expectedCommands += tileset->tilePlan().tilesFadingOut.size();
    };
    addExpectedTilesetCommands(tileset_.get());
    for (const auto& tileset : additionalTilesets_) {
        addExpectedTilesetCommands(tileset.get());
    }
    if (commands.capacity() < expectedCommands) {
        commands.reserve(expectedCommands);
    }
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
    // 0. SkyBox（最远）
    const double skyStartMs = perf::nowMs();
    if (skyBox_ && skyBox_->isReady()) {
        const auto& cam = camera();
        Mat4 vm = cam.viewMatrix();  // must store, .raw() refs internals
        const double* vmPtr = glm::value_ptr(vm.raw());
        float viewMatrix[16];
        for (int i = 0; i < 16; ++i) viewMatrix[i] = static_cast<float>(vmPtr[i]);
        float vpW = static_cast<float>(frameState_.viewportWidthPixels);
        float vpH = static_cast<float>(frameState_.viewportHeightPixels);
        Mat4 pm = cam.projectionMatrix(
            static_cast<double>(vpW), static_cast<double>(vpH));
        const double* pmPtr = glm::value_ptr(pm.raw());
        float projMatrix[16];
        for (int i = 0; i < 16; ++i) projMatrix[i] = static_cast<float>(pmPtr[i]);
        // Night factor: 0 when sun above horizon, ramps to 1 below.
        // In high orbit, keep the starfield visible behind the atmosphere
        // instead of a flat daytime clear color.
        float nightFactor = static_cast<float>(
            skyGradient_->sunElevation() < -0.05
                ? std::clamp(std::exp(skyGradient_->sunElevation() * 8.0), 0.0, 1.0)
                : 0.0);
        double spaceFactor = std::clamp((cam.getHeight() - 120000.0) / 780000.0, 0.0, 1.0);
        spaceFactor = spaceFactor * spaceFactor * (3.0 - 2.0 * spaceFactor);
        nightFactor = std::max(nightFactor, static_cast<float>(spaceFactor));
        commands.push_back(skyBox_->buildCommand(
            viewMatrix, projMatrix, cam.isOrthographic(), nightFactor));
    }
    skyMs = perf::nowMs() - skyStartMs;

    // 0.5 AtmosphereBackgroundPass（SkyBox 之上，地球之下）
    const double atmosphereStartMs = perf::nowMs();
    if (atmospherePass_ && atmospherePass_->isReady()) {
        const auto& cam = camera();
        float vpW = static_cast<float>(frameState_.viewportWidthPixels);
        float vpH = static_cast<float>(frameState_.viewportHeightPixels);

        Vec3 sunDir(
            frameState_.lightDir.x,
            frameState_.lightDir.y,
            frameState_.lightDir.z);

        commands.push_back(atmospherePass_->buildCommand(
            cam.position(),
            static_cast<float>(cam.verticalFovRadians()),
            static_cast<int>(vpW),
            static_cast<int>(vpH),
            cam.right(),
            cam.up(),
            cam.direction(),
            sunDir,
            skyGradient_->parameters()));
    }
    atmosphereMs = perf::nowMs() - atmosphereStartMs;

    // 1. Surface tile rendering
    const double layerCommandsStartMs = perf::nowMs();
    if (tileset_) {
        // Unified Tileset path (cesium-native alignment)
        tileset_->buildRenderCommands(*renderer_, commands);
    }
    for (auto& tileset : additionalTilesets_) {
        if (tileset) {
            tileset->buildRenderCommands(*renderer_, commands);
        }
    }
    layerCommandsMs = perf::nowMs() - layerCommandsStartMs;

    const double fallbackGlobeStartMs = perf::nowMs();
    const bool hasSurfaceTile = std::any_of(commands.begin(), commands.end(),
        [](const RenderCommand& cmd) {
            return cmd.kind == RenderCommandKind::SurfaceTile;
        });
    if (!hasSurfaceTile) {
        commands.insert(commands.begin(), renderer_->makeGlobeCommand(frameState_));
    }
    fallbackGlobeMs = perf::nowMs() - fallbackGlobeStartMs;

    // 2. 矢量图层
    const double vectorCommandsStartMs = perf::nowMs();
    for (auto& vLayer : vectorLayers_) {
        if (vLayer->visible()) {
            vLayer->buildRenderCommands(frameState_, *renderer_, commands);
        }
    }
    vectorCommandsMs = perf::nowMs() - vectorCommandsStartMs;

    // 3. 后处理：为 tile/vector commands 设置 MVP
    const double mvpUniformsStartMs = perf::nowMs();
    if (frameState_.camera) {
        const Camera& cam = *frameState_.camera;
        float vpW = static_cast<float>(frameState_.viewportWidthPixels);
        float vpH = static_cast<float>(frameState_.viewportHeightPixels);

        // cesium-native RTC: compute view and projection in double precision,
        // then bake per-tile origin into the MVP matrix on the CPU.
        const Mat4& viewRaw = cam.viewMatrix();
        const Mat4& projRaw = cam.projectionMatrix(
            static_cast<double>(vpW), static_cast<double>(vpH));
        glm::dmat4 viewD(viewRaw.raw());
        glm::dmat4 projD(projRaw.raw());
        glm::dmat4 viewProj = projD * viewD;

        for (auto& cmd : commands) {
            if (cmd.owner == "globe") continue;
            if (cmd.kind == RenderCommandKind::SurfaceTile && cmd.hasSurfaceTileUniforms) {
                // cesium-native RTC: bake tile origin into MVP via
                // CPU double-precision matrix multiplication.
                // mvp = projection * view * translate(tileOrigin)
                // a_position is relative to tile center (small values).
                glm::dvec3 origin(
                    cmd.surfaceTileOrigin[0],
                    cmd.surfaceTileOrigin[1],
                    cmd.surfaceTileOrigin[2]);
                glm::dmat4 model = glm::translate(glm::dmat4(1.0), origin);
                glm::dmat4 mvp = projD * viewD * model;
                // Convert to float column-major array
                glm::mat4 mvpFloat = glm::mat4(mvp);
                std::memcpy(cmd.surfaceModelViewProjection.data(),
                            glm::value_ptr(mvpFloat),
                            16 * sizeof(float));
                cmd.surfaceLightDir = {
                    frameState_.lightDir.x,
                    frameState_.lightDir.y,
                    frameState_.lightDir.z
                };
                continue;
            }
            if (cmd.kind == RenderCommandKind::GltfPrimitive ||
                cmd.kind == RenderCommandKind::GltfPrimitiveInstanced) {
                glm::dmat4 model(1.0);
                auto originIt = cmd.uniforms.find("u_modelOrigin");
                if (originIt != cmd.uniforms.end() &&
                    originIt->second.size() >= 3) {
                    glm::dvec3 origin(
                        originIt->second[0],
                        originIt->second[1],
                        originIt->second[2]);
                    model = glm::translate(glm::dmat4(1.0), origin);
                }
                glm::dmat4 mvp = viewProj * model;
                glm::mat4 mvpFloat = glm::mat4(mvp);
                auto& mvpU = cmd.uniforms["u_modelViewProjection"];
                mvpU.resize(16);
                std::memcpy(mvpU.data(),
                            glm::value_ptr(mvpFloat),
                            16 * sizeof(float));
                cmd.uniforms["u_lightDir"] = {
                    frameState_.lightDir.x,
                    frameState_.lightDir.y,
                    frameState_.lightDir.z
                };
                if (cmd.hasWorldSortCenter) {
                    const Vec3 center(
                        cmd.worldSortCenter[0],
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
                std::memcpy(mvpU.data(), glm::value_ptr(viewProj), 16 * sizeof(float));
            }
            if (cmd.owner == "surface_tile") {
                cmd.uniforms["u_lightDir"] = {
                    frameState_.lightDir.x,
                    frameState_.lightDir.y,
                    frameState_.lightDir.z
                };
            }
        }
    }
    mvpUniformsMs = perf::nowMs() - mvpUniformsStartMs;

    const double sortStartMs = perf::nowMs();
    bool needsSort = false;
    bool hasTranslucentGltf = false;
    for (size_t i = 1; i < commands.size(); ++i) {
        if (commands[i - 1].blend &&
            (commands[i - 1].kind == RenderCommandKind::GltfPrimitive ||
             commands[i - 1].kind == RenderCommandKind::GltfPrimitiveInstanced)) {
            hasTranslucentGltf = true;
        }
        if (mvpRenderOrder(commands[i - 1].kind) >
            mvpRenderOrder(commands[i].kind)) {
            needsSort = true;
            break;
        }
    }
    if (!commands.empty()) {
        const RenderCommand& last = commands.back();
        if (last.blend &&
            (last.kind == RenderCommandKind::GltfPrimitive ||
             last.kind == RenderCommandKind::GltfPrimitiveInstanced)) {
            hasTranslucentGltf = true;
        }
    }
    if (needsSort || hasTranslucentGltf) {
        sortMvpRenderCommands(commands);
    }
    sortMs = perf::nowMs() - sortStartMs;
    const double surfaceDiagnosticsStartMs = perf::nowMs();
    updateSurfaceCommandDiagnostics(
        commands, frameState_.frameId, frameState_.diagnostics);
    surfaceDiagnosticsMs = perf::nowMs() - surfaceDiagnosticsStartMs;
    const double validateStartMs = perf::nowMs();
    if (auto error = validateMvpRenderCommands(commands, frameState_.frameId)) {
        throw std::runtime_error(
            "MVP render command validation failed for '" + error->owner +
            "': " + error->message);
    }
    validateMs = perf::nowMs() - validateStartMs;

    // 5. 填充诊断数据
    const double diagnosticsStartMs = perf::nowMs();
    auto& diag = frameState_.diagnostics;
    diag.drawCalls = static_cast<int>(commands.size());
    diag.visibleTiles = 0;
    diag.contentTilesets = static_cast<int>(additionalTilesets_.size());
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
    for (const RenderCommand& cmd : commands) {
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
    if (tileset_) {
        const TilePlan& plan = tileset_->tilePlan();
        const TilesetLoadDiagnostics loadDiag = tileset_->loadDiagnostics();
        diag.visibleTiles = static_cast<int>(plan.visibleTiles.size());
        diag.terrainCachedTiles = tileset_->cachedTerrainTiles();
        diag.queuedRequests += loadDiag.loadQueueTotal();
        diag.loadingRequests +=
            loadDiag.pendingTerrainTotal() + loadDiag.pendingContentTotal();
        diag.loadQueuePreloadRequests = loadDiag.loadQueuePreloadRequests;
        diag.loadQueueNormalRequests = loadDiag.loadQueueNormalRequests;
        diag.loadQueueUrgentRequests = loadDiag.loadQueueUrgentRequests;
        diag.pendingTerrainRequests = loadDiag.pendingTerrainRequests;
        diag.pendingTerrainUploads = loadDiag.pendingTerrainUploads;
        diag.pendingTerrainTerminalResults =
            loadDiag.pendingTerrainTerminalResults;
        diag.pendingContentRequests += loadDiag.pendingContentRequests;
        diag.pendingContentUploads += loadDiag.pendingContentUploads;
        diag.pendingContentTerminalResults +=
            loadDiag.pendingContentTerminalResults;
        diag.surfaceMeshBytes = static_cast<int>(tileset_->totalBytesUsed());
        diag.terrainLoadUnloadingTiles = loadDiag.loadUnloadingTiles;
        diag.terrainLoadFailedTemporarilyTiles =
            loadDiag.loadFailedTemporarilyTiles;
        diag.terrainLoadUnloadedTiles = loadDiag.loadUnloadedTiles;
        diag.terrainLoadContentLoadingTiles =
            loadDiag.loadContentLoadingTiles;
        diag.terrainLoadContentLoadedTiles =
            loadDiag.loadContentLoadedTiles;
        diag.terrainLoadDoneTiles = loadDiag.loadDoneTiles;
        diag.terrainLoadFailedTiles = loadDiag.loadFailedTiles;
        diag.terrainContentUnknownTiles = loadDiag.contentUnknownTiles;
        diag.terrainContentEmptyTiles = loadDiag.contentEmptyTiles;
        diag.terrainContentExternalTiles = loadDiag.contentExternalTiles;
        diag.terrainContentRenderTiles = loadDiag.contentRenderTiles;
        diag.terrainUnloadQueueTiles = loadDiag.unloadQueueTiles;
        diag.missingRasterOverlayProjections =
            loadDiag.missingRasterOverlayProjections;
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
    }
    for (const auto& tileset : additionalTilesets_) {
        if (!tileset) continue;
        const TilePlan& plan = tileset->tilePlan();
        const TilesetLoadDiagnostics loadDiag = tileset->loadDiagnostics();
        diag.contentVisibleTiles +=
            static_cast<int>(plan.visibleTiles.size());
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
        diag.terrainContentUnknownTiles += loadDiag.contentUnknownTiles;
        diag.terrainContentEmptyTiles += loadDiag.contentEmptyTiles;
        diag.terrainContentExternalTiles += loadDiag.contentExternalTiles;
        diag.terrainContentRenderTiles += loadDiag.contentRenderTiles;
        diag.surfaceMeshBytes += static_cast<int>(tileset->totalBytesUsed());
    }
    diag.gpuTextureCount = static_cast<int>(surfaceTextures.size());
    if (diag.gpuTextureCount == 0) {
        diag.gpuTextureCount = diag.cachedTextures;
    }
    diag.imageryAttachments =
        diag.imageryExactAttachments + diag.imageryParentFallbackAttachments;
    diagnosticsMs = perf::nowMs() - diagnosticsStartMs;

    // 6. 提交
    frameState_.diagnostics.renderCommandBuildMs =
        perf::nowMs() - renderStartMs;

    const double submitStartMs = perf::nowMs();
    renderer_->submit(commands);
    frameState_.diagnostics.renderSubmitMs = perf::nowMs() - submitStartMs;

    // cesium-native: release tile render references after GPU submit.
    // References were added in Tileset::buildRenderCommands; they must
    // outlive the submit call so the GPU doesn't access freed resources.
    if (tileset_) {
        tileset_->releaseRenderReferences();
    }
    for (auto& tileset : additionalTilesets_) {
        if (tileset) {
            tileset->releaseRenderReferences();
        }
    }

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
        commands.size());
    perf::logTiming(frameState_.frameId,
                    "Scene.render.buildBreakdown",
                    frameState_.diagnostics.renderCommandBuildMs,
                    buildDetail);

    char detail[192];
    std::snprintf(detail, sizeof(detail),
        "build=%.2f submit=%.2f draw=%d surface=%d mesh=%d",
        frameState_.diagnostics.renderCommandBuildMs,
        frameState_.diagnostics.renderSubmitMs,
        frameState_.diagnostics.drawCalls,
        frameState_.diagnostics.renderSurfaceTiles,
        frameState_.diagnostics.surfaceMeshCount);
    perf::logTiming(frameState_.frameId,
                    "Scene.render.total",
                    perf::nowMs() - renderStartMs,
                    detail);
}

void Scene::setTileset(std::unique_ptr<Tileset> tileset) {
    tileset_ = std::move(tileset);
    if (tileset_) {
        if (occlusionCallback_) {
            tileset_->setOcclusionCallback(occlusionCallback_);
        } else {
            tileset_->clearOcclusionCallback();
        }
    }
    configureCameraSurfacePicker();
}

void Scene::addTileset(std::unique_ptr<Tileset> tileset) {
    if (!tileset) return;
    if (occlusionCallback_) {
        tileset->setOcclusionCallback(occlusionCallback_);
    }
    additionalTilesets_.push_back(std::move(tileset));
}

// ---- 矢量图层管理 ----

void Scene::addVectorLayer(std::unique_ptr<VectorLayer> layer) {
    if (!layer) return;
    layer->initialize(renderDevice_);
    vectorLayers_.push_back(std::move(layer));
}

std::unique_ptr<VectorLayer> Scene::removeVectorLayer(const std::string& layerId) {
    auto it = std::find_if(vectorLayers_.begin(), vectorLayers_.end(),
        [&](const auto& l) { return l->id() == layerId; });
    if (it == vectorLayers_.end()) return nullptr;

    auto removed = std::move(*it);
    vectorLayers_.erase(it);
    return removed;
}

// ---- 拾取与选择 ----

PickResult Scene::pick(float screenX, float screenY) const {
    if (!pickingService_ || !camera_) return PickResult{};

    std::function<float(double,double)> terrainSampler;
    if (tileset_) {
        terrainSampler = [this](double lng, double lat) {
            return tileset_->sampleHeight(lng, lat);
        };
    }

    // 先做地形拾取
    PickResult result = pickingService_->pickTerrain(
        screenX, screenY,
        *camera_,
        static_cast<double>(frameState_.viewportWidthPixels),
        static_cast<double>(frameState_.viewportHeightPixels),
        terrainSampler);

    // 再检查矢量图层（替换更近的命中）
    std::vector<const VectorLayer*> layerPtrs;
    for (const auto& l : vectorLayers_) {
        layerPtrs.push_back(l.get());
    }

    auto vecResult = pickingService_->pick(
        screenX, screenY,
        *camera_,
        static_cast<double>(frameState_.viewportWidthPixels),
        static_cast<double>(frameState_.viewportHeightPixels),
        layerPtrs);

    if (vecResult.hitType == PickResult::HitType::VectorFeature &&
        (!result.isValid() || vecResult.distance < result.distance)) {
        result = vecResult;
    }

    return result;
}

void Scene::onHover(const PickResult& result) {
    if (selectionManager_) {
        selectionManager_->onHover(result);
    }
}

void Scene::onSelect(const PickResult& result) {
    if (selectionManager_) {
        selectionManager_->onSelect(result);
    }
}

void Scene::clearSelection() {
    if (selectionManager_) {
        selectionManager_->clearSelection();
    }
}

// ---- 选择回调 ----

void Scene::setupSelectionCallbacks() {
    selectionManager_->setStateChangeCallback(
        [this](const std::string& layerId,
               const std::string& featureId,
               FeatureState state) {
            // 转换 FeatureState → VectorLayer 可理解的字符串
            const char* stateStr = "";
            switch (state) {
                case FeatureState::Hovered:  stateStr = "hover"; break;
                case FeatureState::Selected: stateStr = "selected"; break;
                default: break;
            }
            for (auto& layer : vectorLayers_) {
                if (layer->id() == layerId) {
                    layer->setFeatureState(featureId, stateStr);
                    return;
                }
            }
        });
}

// ---- 输入回调 ----

void Scene::configureCameraSurfacePicker() {
    if (!cameraController_) return;

    cameraController_->setSurfacePicker(
        [this](float screenX, float screenY, Vec3& outPoint) {
            if (!pickingService_ || !camera_) {
                return false;
            }

            PickResult result = pickingService_->pickTerrain(
                screenX, screenY,
                *camera_,
                static_cast<double>(frameState_.viewportWidthPixels),
                static_cast<double>(frameState_.viewportHeightPixels),
                tileset_
                    ? std::function<float(double,double)>(
                          [this](double lng, double lat) {
                              return tileset_->sampleHeight(lng, lat);
                          })
                    : std::function<float(double,double)>{});
            if (!result.isValid()) {
                return false;
            }

            outPoint = result.worldPosition;
            return true;
        });

    cameraController_->setTerrainHeightFunc(
        tileset_
            ? CameraController::TerrainHeightFunc(
                  [this](const Vec3& ecefPosition) -> double {
                      const Cartographic c =
                          Ellipsoid::WGS84().cartesianToCartographic(ecefPosition);
                      return static_cast<double>(
                          tileset_->sampleHeight(c.longitude(), c.latitude()));
                  })
            : CameraController::TerrainHeightFunc{});
}

void Scene::setupInputCallback() {
    inputManager_->setCallback(
        [this](InputManager::Gesture gesture, const InputEvent& event) {
            switch (gesture) {
                case InputManager::Gesture::DragStart:
                    cameraController_->onDragStart(event.screenX, event.screenY,
                                                   event.timestamp);
                    break;
                case InputManager::Gesture::DragMove:
                    cameraController_->onDragMove(event.screenX, event.screenY,
                                                  event.timestamp);
                    break;
                case InputManager::Gesture::DragEnd:
                    cameraController_->onDragEnd();
                    break;
                case InputManager::Gesture::PinchStart:
                    cameraController_->onPinchGesture(event.pinchScale,
                                                      event.screenX,
                                                      event.screenY,
                                                      event.rotationRadians,
                                                      event.centerDeltaX,
                                                      event.centerDeltaY,
                                                      event.timestamp);
                    break;
                case InputManager::Gesture::PinchMove:
                    cameraController_->onPinchGesture(event.pinchScale,
                                                      event.screenX,
                                                      event.screenY,
                                                      event.rotationRadians,
                                                      event.centerDeltaX,
                                                      event.centerDeltaY,
                                                      event.timestamp);
                    break;
                case InputManager::Gesture::PinchEnd:
                    cameraController_->onPinchEnd();
                    break;
                case InputManager::Gesture::Click:
                case InputManager::Gesture::DoubleClick: {
                    PickResult result = pick(event.screenX, event.screenY);
                    if (gesture == InputManager::Gesture::DoubleClick) {
                        if (result.isValid()) {
                            cameraController_->viewDistance(
                                result.worldPosition,
                                result.distance * 0.57);
                        } else {
                            // 以点击位置为中心放大（缩小距离 30%）
                            float newDist = cameraController_->distance() * 0.7f;
                            cameraController_->setDistance(newDist);
                        }
                    } else {
                        // 单击：带修饰键的选择逻辑
                        if (result.isValid()) {
                            if (event.modifiers.shift) {
                                selectionManager_->onSelectAdd(result);
                            } else if (event.modifiers.ctrl || event.modifiers.meta) {
                                selectionManager_->onSelectToggle(result);
                            } else {
                                selectionManager_->onSelect(result);
                            }
                        } else {
                            clearSelection();
                        }
                    }
                    break;
                }
            }
        });
}

bool Scene::pickInteractionFocus(float screenX, float screenY, Vec3& outPoint) const {
    if (!pickingService_ || !camera_) {
        return false;
    }

    const PickResult result = pickingService_->pickTerrain(
        screenX,
        screenY,
        *camera_,
        static_cast<double>(frameState_.viewportWidthPixels),
        static_cast<double>(frameState_.viewportHeightPixels),
        tileset_
            ? std::function<float(double,double)>(
                  [this](double lng, double lat) {
                      return tileset_->sampleHeight(lng, lat);
                  })
            : std::function<float(double,double)>{});
    if (!result.isValid()) {
        return false;
    }

    outPoint = result.worldPosition;
    return true;
}

void Scene::updateInteractionFocus(const InputEvent& event) {
    switch (event.type) {
        case InputEvent::Type::PointerDown:
        case InputEvent::Type::PointerMove:
        case InputEvent::Type::PointerUp:
        case InputEvent::Type::PinchStart:
        case InputEvent::Type::PinchMove:
        case InputEvent::Type::PinchEnd:
            break;
        default:
            return;
    }

    Vec3 focusPoint;
    if (!pickInteractionFocus(event.screenX, event.screenY, focusPoint)) {
        return;
    }
    interactionFocusDirection_ = focusPoint.normalized();
    interactionFocusTimeSeconds_ = elapsedTime_;
    hasInteractionFocus_ = true;
}

void Scene::onInputEvent(const InputEvent& event) {
    updateInteractionFocus(event);
    if (inputManager_) {
        inputManager_->process(event);
    }
}

// ---- 环境系统 ----

void Scene::setTime(double julianDate) {
    timeController_->setJulianDate(julianDate);
}

double Scene::time() const {
    return timeController_->julianDate();
}

void Scene::advanceTime(double seconds) {
    timeController_->advanceSeconds(seconds);
}

Vec3 Scene::sunDirection() const {
    return SunDirection::compute(timeController_->julianDate());
}

} // namespace earth_engine
