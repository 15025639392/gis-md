#include "Engine.h"
#include "scene/Scene.h"
#include "tiling/Tileset.h"
#include "scene/Camera.h"
#include "camera/CameraController.h"
#include "renderer/OffscreenPostProcess.h"
#include "renderer/VirtualTexturePoc.h"
#include "renderer/TerrainPageStore.h"
#include "renderer/TileCompositeBakePoc.h"
#include "renderer/VtIndirectionSamplePoc.h"
#include "renderer/RenderDevice.h"
#include "layers/VectorLayer.h"
#include "layers/ActivatedRasterOverlay.h"  // B2a 门②:overlays.front()->getTileProvider()
#include "debug/PlatformLog.h"
#include "interaction/InputEvent.h"
#include "interaction/PickingService.h"
#include "debug/PerfTimer.h"

#include <chrono>
#include <cstdio>
#include <utility>

namespace earth_engine {

namespace {
const char* diagTagForEffect(OffscreenPostProcess::Effect effect) {
    switch (effect) {
        case OffscreenPostProcess::Effect::Fxaa:
            return "FXAADIAG";
        case OffscreenPostProcess::Effect::AerialFog:
            return "FOGDIAG";
        case OffscreenPostProcess::Effect::Passthrough:
        default:
            return "RTTDIAG";
    }
}
} // namespace

Engine::Engine(RenderDevice* device)
    : device_(device),
      scene_(std::make_unique<Scene>()) {
}

Engine::~Engine() {
    onSurfaceDestroyed();
}

void Engine::onSurfaceCreated() {
    if (!device_) return;

    device_->onSurfaceCreated();
    if (!scene_->setRenderDevice(device_)) {
        surfaceCreated_ = false;
        return;
    }
    surfaceCreated_ = true;
}

void Engine::onSurfaceChanged(int widthPixels, int heightPixels, float dpr) {
    device_->onSurfaceChanged(widthPixels, heightPixels);
    scene_->setViewport(widthPixels, heightPixels, dpr);
    surfaceWidthPixels_ = widthPixels;
    surfaceHeightPixels_ = heightPixels;
}

void Engine::onSurfaceDestroyed() {
    // 离屏资源属于即将失效的 GPU context;surface 重建后惰性重建。
    if (offscreenPostProcess_) {
        offscreenPostProcess_->dispose();
        offscreenPostProcess_.reset();
    }
    offscreenPostProcessInitFailed_ = false;
    if (virtualTexturePoc_) {
        virtualTexturePoc_->dispose();
        virtualTexturePoc_.reset();
    }
    virtualTexturePocInitFailed_ = false;
    if (tileCompositeBakePoc_) {
        tileCompositeBakePoc_->dispose();
        tileCompositeBakePoc_.reset();
    }
    tileCompositeBakePocInitFailed_ = false;
    if (vtIndirectionSamplePoc_) {
        vtIndirectionSamplePoc_->dispose();
        vtIndirectionSamplePoc_.reset();
    }
    vtIndirectionSamplePocInitFailed_ = false;
    if (terrainPageStore_) {
        scene_->setTerrainPageStore(nullptr);
        terrainPageStore_.reset();  // 释放 array 纹理(GPU context 即将失效)
    }
    terrainPageStoreInitFailed_ = false;
    scene_->setRenderDevice(nullptr);
    if (device_) {
        device_->onSurfaceDestroyed();
    }
    surfaceCreated_ = false;
}

void Engine::setOffscreenPassthroughEnabled(bool enabled) {
    offscreenPassthroughEnabled_ = enabled;
    offscreenPostProcessInitFailed_ = false;
}

void Engine::setFxaaEnabled(bool enabled) {
    fxaaEnabled_ = enabled;
    offscreenPostProcessInitFailed_ = false;
}

void Engine::setAerialFogEnabled(bool enabled) {
    aerialFogEnabled_ = enabled;
    offscreenPostProcessInitFailed_ = false;
}

void Engine::setAerialFogParams(float density, float startDistance) {
    aerialFogDensity_ = density;
    aerialFogStartDistance_ = startDistance;
}

void Engine::setVirtualTexturePocEnabled(bool enabled) {
    virtualTexturePocEnabled_ = enabled;
    virtualTexturePocInitFailed_ = false;
    // 关闭时释放资源(避免持有离屏 FBO/atlas 占显存)。
    if (!enabled && virtualTexturePoc_) {
        virtualTexturePoc_->dispose();
        virtualTexturePoc_.reset();
    }
}

void Engine::setTileCompositeBakePocEnabled(bool enabled) {
    tileCompositeBakePocEnabled_ = enabled;
    tileCompositeBakePocInitFailed_ = false;
    if (!enabled && tileCompositeBakePoc_) {
        tileCompositeBakePoc_->dispose();
        tileCompositeBakePoc_.reset();
    }
}

void Engine::setVtIndirectionSamplePocEnabled(bool enabled) {
    vtIndirectionSamplePocEnabled_ = enabled;
    vtIndirectionSamplePocInitFailed_ = false;
    if (!enabled && vtIndirectionSamplePoc_) {
        vtIndirectionSamplePoc_->dispose();
        vtIndirectionSamplePoc_.reset();
    }
}

void Engine::setTerrainPageStoreEnabled(bool enabled) {
    terrainPageStoreEnabled_ = enabled;
    terrainPageStoreInitFailed_ = false;
    if (!enabled && terrainPageStore_) {
        scene_->setTerrainPageStore(nullptr);
        terrainPageStore_.reset();
    }
}

bool Engine::render(double deltaSeconds) {
    if (!surfaceCreated_ || !isReady()) {
        fprintf(stderr,
                "[Engine::render] BLOCKED: surface=%d ready=%d\n",
                surfaceCreated_,
                isReady());
        return false;
    }
    const double frameStartMs = perf::nowMs();

    // 自动计时
    if (deltaSeconds <= 0.0) {
        auto now = std::chrono::steady_clock::now();
        double nowSec = std::chrono::duration<double>(
            now.time_since_epoch()).count();
        if (lastRenderTime_ > 0.0) {
            deltaSeconds = nowSec - lastRenderTime_;
        } else {
            deltaSeconds = 1.0 / 60.0;
        }
        lastRenderTime_ = nowSec;
    }

    {
        // Update first so this frame's FrameState (camera + sky clear color) is
        // ready before beginFrame() clears the color/depth attachments. GPU
        // uploads during update use createTexture/createBuffer, which need no
        // active frame on either backend, so running update ahead of beginFrame
        // is safe.
        const double startMs = perf::nowMs();
        scene_->update(deltaSeconds);
        scene_->recordEngineTiming(
            Scene::EngineTimingScope::SceneUpdate,
            perf::nowMs() - startMs);
    }
    // 北极星 VT PoC(测量台专用,默认关):在场景 update 后、主 draw 前跑一帧
    // feedback→回读→页表整链,量移动端固定开销。任何一环失败都短路,绝不影响
    // 生产渲染。lastStats() 在帧尾并进 EarthPerf 头行。
    if (virtualTexturePocEnabled_ && !virtualTexturePocInitFailed_) {
        if (!virtualTexturePoc_) {
            auto poc = std::make_unique<VirtualTexturePoc>();
            if (poc->initialize(device_, VirtualTexturePocConfig{})) {
                virtualTexturePoc_ = std::move(poc);
            } else {
                virtualTexturePocInitFailed_ = true;
            }
        }
        if (virtualTexturePoc_ &&
            virtualTexturePoc_->ensureResources(surfaceWidthPixels_,
                                                surfaceHeightPixels_)) {
            virtualTexturePoc_->tick();
        }
    }
    // 北极星 B 方案 PoC(测量台专用,默认关):对当前可见瓦片数做 N 个离屏 bake
    // pass,量 B 的每帧烘焙开销(与 C 回读税对比)。瓦片数取上帧诊断(冻结相机
    // settled 下逐帧一致)。同样短路、纯旁路,不影响渲染。
    if (tileCompositeBakePocEnabled_ && !tileCompositeBakePocInitFailed_) {
        if (!tileCompositeBakePoc_) {
            auto poc = std::make_unique<TileCompositeBakePoc>();
            if (poc->initialize(device_, TileCompositeBakePocConfig{})) {
                tileCompositeBakePoc_ = std::move(poc);
            } else {
                tileCompositeBakePocInitFailed_ = true;
            }
        }
        if (tileCompositeBakePoc_ && tileCompositeBakePoc_->ensureResources()) {
            tileCompositeBakePoc_->tick(scene_->diagnostics().visibleTiles);
        }
    }
    // 北极星 合成方案 门① 原型(测量台专用,默认关):一屏 fill 量逐片元间接采样
    // 倍率(baseline vs descent)。同样短路、纯旁路,不影响渲染。
    if (vtIndirectionSamplePocEnabled_ && !vtIndirectionSamplePocInitFailed_) {
        if (!vtIndirectionSamplePoc_) {
            auto poc = std::make_unique<VtIndirectionSamplePoc>();
            if (poc->initialize(device_, VtIndirectionSamplePocConfig{})) {
                vtIndirectionSamplePoc_ = std::move(poc);
            } else {
                vtIndirectionSamplePocInitFailed_ = true;
            }
        }
        if (vtIndirectionSamplePoc_ &&
            vtIndirectionSamplePoc_->ensureResources(surfaceWidthPixels_,
                                                     surfaceHeightPixels_)) {
            vtIndirectionSamplePoc_->tick();
        }
    }
    // 北极星 合成方案 门③ Step3 页存储原型(默认关):建 texture2DArray 页存储
    // (Step3a 合成图案填充一次)并挂到内部 Renderer;GltfDrawCommandBuilder 对
    // 目标 capped 真实地形瓦片挂 array + 门控采样。挂上后持久生效(下帧起应用)。
    if (terrainPageStoreEnabled_ && !terrainPageStoreInitFailed_) {
        if (!terrainPageStore_) {
            auto store = std::make_unique<TerrainPageStore>();
            if (store->initialize(device_, TerrainPageStore::Config{})) {
                terrainPageStore_ = std::move(store);
                scene_->setTerrainPageStore(terrainPageStore_.get());
            } else {
                terrainPageStoreInitFailed_ = true;
            }
        }
        // 渲染线程驱动:目标锁定后 kick 异步影像 fetch + 排空已到达影像灌 layer。
        if (terrainPageStore_) {
            // 北极星 SVT B2a 门②:选择完成(FrameState + tilePlan 已填)后、tick 前,
            // 跑「屏幕可见影像页 determination」并插桩(纯读 + log,不碰池/fetch/render)。
            // 主相机视图 + 本帧可见瓦片 + 影像 provider 三者齐备才跑;否则跳过。
            const FrameState& frameState = scene_->frameState();
            Tileset* tileset = scene_->tileset();
            if (!frameState.selectorViews.empty() && tileset != nullptr) {
                const std::vector<ActivatedRasterOverlay*>& overlays =
                    tileset->rasterOverlays();
                RasterOverlayTileProvider* pageProvider =
                    overlays.empty() ? nullptr
                                     : overlays.front()->getTileProvider();
                if (pageProvider != nullptr) {
                    terrainPageStore_->updateVisiblePages(
                        frameState.selectorViews.front(),
                        tileset->tilePlan().tilesToRenderThisFrame,
                        pageProvider,
                        tileset->maximumScreenSpaceError());
                }
            }
            terrainPageStore_->tick();
        }
    }
    if (scene_->shouldHoldPresentationFrame()) {
        scene_->recordEngineTiming(Scene::EngineTimingScope::BeginFrame, 0.0);
        scene_->recordEngineTiming(Scene::EngineTimingScope::SceneRender, 0.0);
        scene_->recordEngineTiming(Scene::EngineTimingScope::EndFrame, 0.0);
        scene_->finishEngineFrame(perf::nowMs() - frameStartMs);
        perf::logTiming(scene_->frameState().frameId,
                        "Engine.render.total",
                        scene_->diagnostics().engineFrameCpuMs,
                        "hold=1 draw=0 tiles=0");
        return false;
    }
    {
        const double startMs = perf::nowMs();
        // Push this frame's sky clear color into the device before beginFrame()
        // performs the clear (replaces the previously hardcoded sky-blue).
        float clearR, clearG, clearB, clearA;
        getClearColor(clearR, clearG, clearB, clearA);
        device_->setClearColor(clearR, clearG, clearB, clearA);
        device_->beginFrame();
        // 离屏后处理(flag ON 且资源可用时):场景 pass 的目标换成离屏
        // FBO,场景后追加全屏后处理 pass 上屏;任何一环失败都回落直绘。
        // 优先级:AerialFog > FXAA > passthrough 调试直通。
        const bool wantOffscreen =
            aerialFogEnabled_ || fxaaEnabled_ || offscreenPassthroughEnabled_;
        const OffscreenPostProcess::Effect wantEffect =
            aerialFogEnabled_ ? OffscreenPostProcess::Effect::AerialFog
            : fxaaEnabled_    ? OffscreenPostProcess::Effect::Fxaa
                              : OffscreenPostProcess::Effect::Passthrough;
        // 期望的 effect 变了(运行时切换)→ 丢弃旧对象按新 shader 重建。
        if (offscreenPostProcess_ &&
            offscreenPostProcess_->effect() != wantEffect) {
            offscreenPostProcess_->dispose();
            offscreenPostProcess_.reset();
            offscreenPostProcessInitFailed_ = false;
        }
        if (wantOffscreen && !offscreenPostProcessInitFailed_ &&
            !offscreenPostProcess_) {
            auto postProcess = std::make_unique<OffscreenPostProcess>();
            if (postProcess->initialize(device_, wantEffect)) {
                offscreenPostProcess_ = std::move(postProcess);
            } else {
                offscreenPostProcessInitFailed_ = true;
            }
        }
        Framebuffer* offscreenTarget = nullptr;
        if (wantOffscreen && offscreenPostProcess_) {
            offscreenTarget = offscreenPostProcess_->ensureFramebuffer(
                surfaceWidthPixels_, surfaceHeightPixels_);
        }
        // 场景 pass(离屏或直绘主 pass)。beginFrame 只做帧获取,pass 的
        // clear + 状态设置在 beginPass 里;跳帧时返回 false,submit 自身
        // 对无 encoder 空判,scene->render() 的 CPU 侧工作照常推进。
        offscreenPassActive_ =
            offscreenTarget && device_->beginPass(offscreenTarget);
        if (!offscreenPassActive_) {
            device_->beginPass(nullptr);
        }
        scene_->recordEngineTiming(
            Scene::EngineTimingScope::BeginFrame,
            perf::nowMs() - startMs);
    }
    bool scenePresented = false;
    {
        const double startMs = perf::nowMs();
        scenePresented = scene_->render();
        scene_->recordEngineTiming(
            Scene::EngineTimingScope::SceneRender,
            perf::nowMs() - startMs);
    }
    {
        const double startMs = perf::nowMs();
        device_->endPass();
        if (offscreenPassActive_ && scenePresented) {
            // aerial fog 每帧参数:near/far + 相机基 + 太阳,喂给 shader 逐
            // 像素算天空色作雾色(与大气 pass 同源→随高度/方向/太阳自然同调),
            // 密度随高度/视角在 shader 内衰减。密度/起点走 SDK 可配值。
            OffscreenPostProcess::FrameParams params;
            const Camera& cam = scene_->camera();
            params.nearPlane = static_cast<float>(cam.nearPlaneMeters());
            params.farPlane = static_cast<float>(cam.farPlaneMeters());
            params.fogDensity = aerialFogDensity_;
            params.fogStartDistance = aerialFogStartDistance_;
            auto toArr = [](const Vec3& v) {
                return std::array<float, 3>{static_cast<float>(v.x()),
                                            static_cast<float>(v.y()),
                                            static_cast<float>(v.z())};
            };
            params.camPos = toArr(cam.position());
            params.camRight = toArr(cam.right());
            params.camUp = toArr(cam.up());
            params.camForward = toArr(cam.direction());
            params.sunDir = toArr(scene_->sunDirection());
            params.tanFovHalf =
                static_cast<float>(std::tan(cam.verticalFovRadians() * 0.5));
            params.aspect = surfaceHeightPixels_ > 0
                ? static_cast<float>(surfaceWidthPixels_) /
                      static_cast<float>(surfaceHeightPixels_)
                : 1.0f;
            // 密度高度衰减用的星球半径:椭球赤道半径(近似,雾密度对此不敏感)。
            params.planetRadius = 6378137.0f;
            if (device_->beginPass(nullptr)) {
                device_->submit({offscreenPostProcess_->buildCommand(params)});
                device_->endPass();
            }
            static int postDiagCounter = 0;
            if (++postDiagCounter % 120 == 1) {
                platformLog(LogLevel::Info,
                            diagTagForEffect(offscreenPostProcess_->effect()),
                            "offscreenPass=1 postProcess=1 fbo=%dx%d",
                            surfaceWidthPixels_, surfaceHeightPixels_);
            }
        }
        device_->endFrame();
        scene_->recordEngineTiming(
            Scene::EngineTimingScope::EndFrame,
            perf::nowMs() - startMs);
    }

    scene_->finishEngineFrame(perf::nowMs() - frameStartMs);
    const Diagnostics& diag = scene_->diagnostics();
    // 北极星 VT PoC 头行段(仅在 PoC 活跃时追加,默认关时为空 → 零污染):
    //   vtReadback = **①的核心固定开销数**(回读 stall);vtFeedback/vtUpdate 为
    //   feedback pass CPU 侧与解码+页表耗时;vis/res 为可见/驻留页;atlas=固定占用KB。
    char vtDetail[192] = "";
    if (virtualTexturePoc_ && virtualTexturePoc_->isReady()) {
        const VirtualTexturePocFrameStats& s = virtualTexturePoc_->lastStats();
        std::snprintf(vtDetail, sizeof(vtDetail),
            " vtAsync=%d vtReadback=%.3f vtEnqueue=%.3f vtFeedback=%.3f vtUpdate=%.3f vtVis=%d vtRes=%d vtPend=%d vtAtlasKB=%lld",
            s.async ? 1 : 0, s.readbackMs, s.enqueueMs, s.feedbackPassMs,
            s.updateMs, s.visiblePages, s.residentPages, s.readbackPending ? 1 : 0,
            static_cast<long long>(virtualTexturePoc_->atlasBytes() / 1024));
    }
    // 北极星 B 方案 PoC 头行段(仅活跃时追加):bBake=每帧 N 个逐瓦片 bake pass
    //   总耗时(**B 的核心每帧成本**,vs C 的 vtReadback);bTiles=烘焙瓦片数;
    //   bMemKB=B 逐瓦片纹理内存需求。
    char bDetail[96] = "";
    if (tileCompositeBakePoc_ && tileCompositeBakePoc_->isReady()) {
        const TileCompositeBakeFrameStats& b = tileCompositeBakePoc_->lastStats();
        std::snprintf(bDetail, sizeof(bDetail),
            " bBake=%.3f bTiles=%d bMemKB=%lld",
            b.bakeMs, b.bakedTiles,
            static_cast<long long>(b.bakeBytes / 1024));
    }
    // 北极星 门① 原型头行段(仅活跃时追加):**逐片元间接降深度曲线**——vtiBase=
    //   无间接的 1 次 atlas 采样每-pass GPU fill ms(参照地板);vtiD{n}=降 n 层
    //   (n 依赖 fetch + atlas 采样)每-pass ms;vtiSync=强制同步地板(已减去);
    //   vtiMP=每 pass fill 的百万片元数。倍率自算 vtiDn/vtiBase。
    char viDetail[240] = "";
    if (vtIndirectionSamplePoc_ && vtIndirectionSamplePoc_->isReady()) {
        const VtIndirectionSampleFrameStats& v =
            vtIndirectionSamplePoc_->lastStats();
        const int* d = vtSweepDepths();
        std::snprintf(viDetail, sizeof(viDetail),
            " vtiBase=%.3f vtiD%d=%.3f vtiD%d=%.3f vtiD%d=%.3f vtiD%d=%.3f vtiD%d=%.3f vtiD%d=%.3f vtiSync=%.3f vtiMP=%.2f",
            v.baselineMs,
            d[0], v.descentMs[0], d[1], v.descentMs[1], d[2], v.descentMs[2],
            d[3], v.descentMs[3], d[4], v.descentMs[4], d[5], v.descentMs[5],
            v.syncFloorMs, static_cast<double>(v.fillPixels) / 1.0e6);
    }
    char detail[800];
    std::snprintf(detail, sizeof(detail),
        "begin=%.2f update=%.2f render=%.2f submit=%.2f end=%.2f draw=%d tiles=%d hold=%d%s%s%s",
        diag.engineBeginFrameMs,
        diag.sceneUpdateMs,
        diag.sceneRenderMs,
        diag.renderSubmitMs,
        diag.engineEndFrameMs,
        diag.drawCalls,
        diag.visibleTiles,
        scenePresented ? 0 : 1,
        vtDetail,
        bDetail,
        viDetail);
    perf::logTiming(scene_->frameState().frameId,
                    "Engine.render.total",
                    diag.engineFrameCpuMs,
                    detail);
    return scenePresented;
}

void Engine::onInputEvent(const InputEvent& event) {
    scene_->onInputEvent(event);
}

void Engine::onDragStart(float xPixels, float yPixels) {
    InputEvent event;
    event.type = InputEvent::Type::PointerDown;
    event.screenX = xPixels;
    event.screenY = yPixels;
    event.pointerType = InputEvent::PointerType::Touch;
    onInputEvent(event);
}

void Engine::onDragMove(float xPixels, float yPixels) {
    InputEvent event;
    event.type = InputEvent::Type::PointerMove;
    event.screenX = xPixels;
    event.screenY = yPixels;
    event.pointerType = InputEvent::PointerType::Touch;
    onInputEvent(event);
}

void Engine::onDragEnd() {
    InputEvent event;
    event.type = InputEvent::Type::PointerUp;
    event.pointerType = InputEvent::PointerType::Touch;
    onInputEvent(event);
}

Camera& Engine::camera() {
    return scene_->camera();
}

CameraController& Engine::cameraController() {
    return scene_->cameraController();
}

// ---- 矢量图层 ----

void Engine::addVectorLayer(std::unique_ptr<VectorLayer> layer) {
    scene_->addVectorLayer(std::move(layer));
}

std::unique_ptr<VectorLayer> Engine::removeVectorLayer(const std::string& layerId) {
    return scene_->removeVectorLayer(layerId);
}

size_t Engine::vectorLayerCount() const {
    return scene_->vectorLayerCount();
}

void Engine::setTileset(std::unique_ptr<Tileset> tileset) {
    scene_->setTileset(std::move(tileset));
}

void Engine::stageTilesetReplacement(std::unique_ptr<Tileset> tileset) {
    scene_->stageTilesetReplacement(std::move(tileset));
}

void Engine::addTileset(std::unique_ptr<Tileset> tileset) {
    scene_->addTileset(std::move(tileset));
}

void Engine::setSelectorViewOverride(
    std::vector<SelectorView> selectorViews) {
    scene_->setSelectorViewOverride(std::move(selectorViews));
}

void Engine::clearSelectorViewOverride() {
    scene_->clearSelectorViewOverride();
}

void Engine::setOcclusionCallback(TileOcclusionCallback callback) {
    scene_->setOcclusionCallback(std::move(callback));
}

void Engine::clearOcclusionCallback() {
    scene_->clearOcclusionCallback();
}

bool Engine::hasTerrain() const {
    return scene_->hasTerrain();
}

// ---- 拾取与选择 ----

PickResult Engine::pick(float screenX, float screenY) const {
    return scene_->pick(screenX, screenY);
}

void Engine::onHover(const PickResult& result) {
    scene_->onHover(result);
}

void Engine::onSelect(const PickResult& result) {
    scene_->onSelect(result);
}

void Engine::clearSelection() {
    scene_->clearSelection();
}

// ---- 环境系统 ----

void Engine::setTime(double julianDate) {
    scene_->setTime(julianDate);
}

double Engine::time() const {
    return scene_->time();
}

void Engine::advanceTime(double seconds) {
    scene_->advanceTime(seconds);
}

Vec3 Engine::sunDirection() const {
    return scene_->sunDirection();
}

bool Engine::debugAnchorWorld(Vec3& outWorld) const {
    if (!scene_) return false;
    return scene_->cameraController().debugAnchorWorld(outWorld);
}

double Engine::cameraHeadingRadians() const {
    return scene_ ? scene_->cameraController().headingRadians() : 0.0;
}

void Engine::resetNorthUp() {
    if (scene_) scene_->cameraController().resetNorthUp();
}

void Engine::getClearColor(float& r, float& g, float& b, float& a) const {
    const auto& fs = scene_->frameState();
    r = fs.clearR;
    g = fs.clearG;
    b = fs.clearB;
    a = fs.clearA;
}

const Diagnostics& Engine::diagnostics() const {
    return scene_->diagnostics();
}

const PresentationTrace& Engine::presentationTrace() const {
    return scene_->presentationTrace();
}

bool Engine::isReady() const {
    return scene_ && scene_->isReady();
}

} // namespace earth_engine
