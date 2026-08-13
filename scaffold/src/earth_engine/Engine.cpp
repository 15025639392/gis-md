#include "Engine.h"
#include "scene/Scene.h"
#include "tiling/Tileset.h"
#include "scene/Camera.h"
#include "camera/CameraSystem.h"
#include "renderer/OffscreenPostProcess.h"
#include "renderer/PipelineConfig.h"
#include "renderer/VirtualTexturePoc.h"
#include "renderer/TerrainPageStore.h"
#include "tiling/TerrainDisplacementTemplatePool.h"
#include "renderer/TileCompositeBakePoc.h"
#include "renderer/VtIndirectionSamplePoc.h"
#include "renderer/RenderDevice.h"
#include "layers/FeatureRenderLayer.h"
#include "layers/VectorLayer.h"
#include "layers/ActivatedRasterOverlay.h"  // B2a 门②:overlays.front()->getTileProvider()
#include "core/cache/HttpCache.h"
#include "debug/Contracts.h"
#include "debug/Policies.h"
#include "debug/PlatformLog.h"
#include "interaction/InputEvent.h"
#include "interaction/PickingService.h"
#include "debug/EnvSnapshot.h"
#include "debug/PerfTimer.h"
#include "threading/RenderThreadPlacement.h"

#include <chrono>
#include <climits>
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
        case OffscreenPostProcess::Effect::Tonemap:
            return "HDRDIAG";
        case OffscreenPostProcess::Effect::AerialFogTonemap:
            return "HDRFOGDIAG";
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
    if (terrainDisplacementPool_) {
        scene_->setTerrainDisplacementPool(nullptr);
        terrainDisplacementPool_.reset();  // 释放模板 VBO/IBO(GPU context 失效)
    }
    scene_->setRenderDevice(nullptr);
    if (device_) {
        device_->onSurfaceDestroyed();
    }
    surfaceCreated_ = false;
}

bool Engine::offscreenPostProcessSupported() const {
    return device_ && device_->supportsOffscreenPostProcess();
}

bool Engine::setOffscreenPassthroughEnabled(bool enabled) {
    if (enabled && !offscreenPostProcessSupported()) {
        platformLog(LogLevel::Error, "Engine",
                    "offscreen passthrough requested but backend does not "
                    "support offscreen post-process; request ignored");
        offscreenPassthroughEnabled_ = false;
        return false;
    }
    offscreenPassthroughEnabled_ = enabled;
    offscreenPostProcessInitFailed_ = false;
    return true;
}

bool Engine::setFxaaEnabled(bool enabled) {
    if (enabled && !offscreenPostProcessSupported()) {
        platformLog(LogLevel::Error, "Engine",
                    "FXAA requested but backend does not support offscreen "
                    "post-process; request ignored");
        fxaaEnabled_ = false;
        return false;
    }
    fxaaEnabled_ = enabled;
    offscreenPostProcessInitFailed_ = false;
    return true;
}

bool Engine::setAerialFogEnabled(bool enabled) {
    if (enabled && !offscreenPostProcessSupported()) {
        platformLog(LogLevel::Error, "Engine",
                    "aerial fog requested but backend does not support "
                    "offscreen post-process; request ignored");
        aerialFogEnabled_ = false;
        return false;
    }
    aerialFogEnabled_ = enabled;
    offscreenPostProcessInitFailed_ = false;
    return true;
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

bool Engine::setGpuPassTimingEnabled(bool enabled) {
    if (!device_) {
        gpuPassTimingEnabled_ = false;
        return false;
    }
    // 后端返回的是**实际**状态:扩展缺失时请求开也是关。两者症状相同(日志里
    // 一行 GpuPass 都没有),不把这个区别落到返回值/日志上就只能靠猜。
    gpuPassTimingEnabled_ = device_->setGpuTimingEnabled(enabled);
    if (enabled && !gpuPassTimingEnabled_) {
        platformLog(LogLevel::Warning, "GpuPass",
                    "GPU 区间计时请求开启,但后端不支持 → 保持关闭");
    }
    return gpuPassTimingEnabled_;
}

void Engine::logGpuPassTiming() {
    if (!gpuPassTimingEnabled_ || !device_) return;
    const GpuFrameTiming* timing = device_->lastGpuFrameTiming();
    if (!timing || timing->frameId == lastLoggedGpuFrameId_) return;
    lastLoggedGpuFrameId_ = timing->frameId;
    // 回读滞后数帧,每帧都有新结果;按结果计数节流到 ~1 秒一行。
    if ((++gpuPassResultCount_ % 60) != 1) return;
    if (timing->disjoint) {
        platformLog(LogLevel::Info, "GpuPass",
                    "frame=%llu DISJOINT(GPU 被抢占/调频,本帧读数已作废)",
                    static_cast<unsigned long long>(timing->frameId));
        return;
    }
    char line[512];
    int written = std::snprintf(line, sizeof(line),
        "frame=%llu total=%.2fms dropped=%d |",
        static_cast<unsigned long long>(timing->frameId),
        timing->totalMs,
        timing->droppedRegions);
    for (const GpuFrameTiming::Region& r : timing->regions) {
        if (written < 0 || written >= static_cast<int>(sizeof(line))) break;
        written += std::snprintf(line + written, sizeof(line) - written,
                                 " %s=%.2f(x%d)", r.name.c_str(), r.ms, r.count);
    }
    platformLog(LogLevel::Info, "GpuPass", "%s", line);
}

void Engine::setFrameGatingEnabled(bool enabled) {
    frameGatingEnabled_ = enabled;
    // 关→开、开→关都从"要画"起步:关闭时若停在空闲态,没人再来置脏位。
    requestRender("gatingToggled");
}

void Engine::requestRender(const char* reason) {
    renderRequestReason_.store(reason ? reason : "unknown",
                               std::memory_order_relaxed);
    renderRequested_.store(true, std::memory_order_release);
}

namespace {
/// 自检窗口帧数。20 帧 ≈ 0.33s @60fps:够长到能等到"刚落地但慢一步"的产物,
/// 又短到不会把设备按在满帧率上。
constexpr int kShadowVerifySampleFrames = 20;
}  // namespace

bool Engine::needsFrame() {
    // 并行验证期:每帧对拍令牌账与旧判据(见 Scene::auditWorkLedger)。
    // 放在 gating 早退**之前** —— 关掉 gating 时同样要能收集分歧。
    if (scene_) scene_->auditWorkLedger();
    // CPU 常驻账量测(限频 300 帧一行,tag=CpuAcct)
    if (scene_) scene_->logCpuResidentAccount();
    if (!frameGatingEnabled_) return true;

    // 停帧前的余量帧:子系统"这一帧报干净"与"画面已经稳定"常差一两帧(上传
    // 在本帧末落地、下一帧才画得出来)。没有余量会停在倒数第二帧上,症状是
    // 「最后一块瓦片永远不出现」。
    constexpr int kSettleFrames = 3;

    const char* reason = nullptr;
    bool needs = true;
    const char* convergingReason = nullptr;
    // 事件型:exchange 消费一次。**必须消费**,否则一次输入让循环永远跑下去。
    if (renderRequested_.exchange(false, std::memory_order_acq_rel)) {
        reason = renderRequestReason_.load(std::memory_order_relaxed);
        settleFrames_ = kSettleFrames;
    } else if (!lastFramePresented_) {
        // 上一帧被 presentation hold 扣住没呈现。hold 的活性兜底是"连续扣住
        // N 帧后强制呈现"的计数器,而计数器只在 render() 里前进 —— 停帧会让
        // 它永远停在原地,hold 从"暂态"变成"永久黑屏"。
        reason = "held";
        settleFrames_ = kSettleFrames;
    } else if (scene_ && scene_->hasConvergingWork(&convergingReason)) {
        reason = convergingReason;
        settleFrames_ = kSettleFrames;
    } else if (settleFrames_ > 0) {
        --settleFrames_;
        reason = "settle";
    } else if (shadowVerifyEnabled_ && !shadowVerifyDoneThisIdle_) {
        // 影子渲染自检:本该睡了,先多渲几帧看画面还变不变。
        // 每个 idle 段只做一次(shadowVerifyDoneThisIdle_ 在任何"重新变忙"
        // 的分支里清掉),否则自检本身会把设备按在 60fps 上。
        constexpr int kShadowVerifyFrames = kShadowVerifySampleFrames;
        if (shadowVerifyFramesLeft_ == 0) {
            shadowVerifyFramesLeft_ = kShadowVerifyFrames;
            shadowVerifyBaseline_.clear();
            shadowVerifyMismatches_ = 0;
        }
        reason = "shadowVerify";
    } else {
        reason = "idle";
        needs = false;
    }
    if (needs && reason && std::strcmp(reason, "shadowVerify") != 0) {
        // 重新变忙 → 本轮 idle 的自检作废,下次进 idle 再来一次。
        shadowVerifyDoneThisIdle_ = false;
        shadowVerifyFramesLeft_ = 0;
    }
    // 进/出空闲各打一行。没有这两行,"停帧了"和"卡死了"在 logcat 里读数完全
    // 相同 —— 都是"什么都不打"。
    // 醒着时也要周期性报 reason。只在转换时打行,"忙着所以醒着"与"某个判据卡住
    // 所以醒着"在日志里读数完全相同(都是没有新行)—— 而后者是 gating 收益被
    // 悄悄吃掉的唯一形态。~2 秒一行,静止期本就不会触发(空闲不进这条)。
    if (needs) {
        if ((framesAwake_++ % 120) == 0) {
            platformLog(LogLevel::Info, "FrameGate", "awake reason=%s",
                        reason ? reason : "?");
        }
    } else {
        framesAwake_ = 0;
    }
    if (!needs && !wasIdle_) {
        platformLog(LogLevel::Info, "FrameGate",
                    "idle after frame=%llu",
                    static_cast<unsigned long long>(
                        scene_ ? scene_->frameState().frameId : 0));
        wasIdle_ = true;
    } else if (needs && wasIdle_) {
        platformLog(LogLevel::Info, "FrameGate", "wake reason=%s",
                    reason ? reason : "?");
        wasIdle_ = false;
    }
    return needs;
}

void Engine::setTerrainPageStoreEnabled(bool enabled) {
    terrainPageStoreEnabled_ = enabled;
    terrainPageStoreInitFailed_ = false;
    if (!enabled && terrainPageStore_) {
        scene_->setTerrainPageStore(nullptr);
        terrainPageStore_.reset();
    }
}

Renderer* Engine::renderer() const {
    return scene_ ? scene_->renderer() : nullptr;
}

void Engine::setGpuHeightBakeEnabled(bool enabled) {
    gpuHeightBakeEnabled_ = enabled;
    // pool 可能尚未创建(config 先于 eager pool 建时应用)——两个 pool 创建点也会
    // 按 gpuHeightBakeEnabled_ 应用,故此处只需转发给已存在的 pool。
    if (terrainDisplacementPool_) {
        terrainDisplacementPool_->setGpuHeightBakeEnabled(enabled);
    }
}

void Engine::setTerrainGpuDisplacementEnabled(bool enabled) {
    terrainGpuDisplacementEnabled_ = enabled;
    Tileset* tileset = scene_ ? scene_->tileset() : nullptr;
    if (enabled) {
        if (!terrainDisplacementPool_ && device_) {
            // 急切创建 pool(而非等 render() 里惰性建):render() 的惰性块在
            // scene_->update()(瓦片选择+重建 draw 命令)之后,导致首帧瓦片 build
            // 时 pool 仍为空、命令缓存成 CPU 路径,之后不再重建 → GPU 位移永不
            // 生效。本函数在 render 线程调用(GL context 已就绪),此处建可保证
            // pool 先于任何瓦片 build 存在。
            terrainDisplacementPool_ =
                std::make_unique<TerrainDisplacementTemplatePool>();
            terrainDisplacementPool_->initialize(device_);
            terrainDisplacementPool_->setGpuHeightBakeEnabled(gpuHeightBakeEnabled_);
            scene_->setTerrainDisplacementPool(terrainDisplacementPool_.get());
        }
        // 失效已缓存的地形命令 → 下帧全部按 GPU 位移路径重建(否则开关前已加载
        // 的瓦片永远停留在 CPU baked VBO,运行时开关只对新瓦片生效)。
        if (tileset) {
            tileset->invalidateTerrainDrawCommands();
        }
    } else if (terrainDisplacementPool_) {
        // 关闭:先失效命令(清掉命令对 pool GPU buffer 的裸指针),再释放 pool,
        // 消除 ON→OFF 悬垂句柄崩溃;命令随后按 CPU baked VBO 路径重建。
        // P5b:模板活跃期间加载的 fine 地形瓦片没有 per-tile VBO(有意跳过),
        // 关闭后其命令无几何可绑会被 draw builder 兜底丢弃 → 标记内容资源脏,
        // 让 prepare 按「模板不活跃」重走 allReady(sharedTemplateGeometry 不再
        // 视为就绪)→ 重建 legacy VBO 补洞。
        if (tileset) {
            tileset->invalidateTerrainDrawCommands();
            tileset->markContentResourcesDirty();
            // 幽灵网格摘除后 CPU 顶点已释放,markContentResourcesDirty 让
            // prepare 重走 legacy VBO 也建不出来(无顶点可建)——这些瓦片必须
            // 整个重载。不做的话关位移池 = 已加载地形永久消失。
            const std::size_t reloaded =
                tileset->reloadGhostReleasedTerrainContent(nullptr);
            if (reloaded > 0) {
                platformLog(LogLevel::Info, "Terrain",
                            "displacement pool OFF: reloading %zu "
                            "ghost-released terrain tiles",
                            reloaded);
            }
        }
        scene_->setTerrainDisplacementPool(nullptr);
        terrainDisplacementPool_.reset();
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
        // 帧级按需渲染开着时,两帧之间可能隔了**几十秒**的空闲。原样喂下去,
        // 所有按 dt 积分的东西都会一步跳完:惯性一帧甩出去、geomorph 直接跳到
        // 终态、帧预算的时间片判定认为这一帧早已超支(于是本帧一个加载都不发)。
        // 这不是"数值大一点",是让恢复的第一帧变成一次系统性的行为异常。
        // 钳到一个正常帧长上界:空闲期本来就没有需要补上的动画进度。
        constexpr double kMaxAutoDeltaSeconds = 1.0 / 15.0;
        if (deltaSeconds > kMaxAutoDeltaSeconds) {
            deltaSeconds = kMaxAutoDeltaSeconds;
        }
    }

    // 北极星 Phase 2c 地形 GPU 位移(默认开,P5):惰性建共享位移模板池并挂到内部
    // Renderer。**必须在 scene_->update() 之前**——draw 命令的模板 swap 在命令 build
    // 时(update 内)定型,pool 晚于首次 update 建则首帧命令缓存成 CPU baked 路径且不
    // 再重建 → GPU 位移永不生效(运行时 toggle 靠 invalidateTerrainDrawCommands 兜底,
    // 但 default-on 从启动即需 pool 先于首个瓦片 build 存在)。
    if (terrainGpuDisplacementEnabled_ && !terrainDisplacementPool_) {
        terrainDisplacementPool_ =
            std::make_unique<TerrainDisplacementTemplatePool>();
        terrainDisplacementPool_->initialize(device_);
        terrainDisplacementPool_->setGpuHeightBakeEnabled(gpuHeightBakeEnabled_);
        scene_->setTerrainDisplacementPool(terrainDisplacementPool_.get());
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
    double pageStoreMs = 0.0;
    if (terrainPageStoreEnabled_ && !terrainPageStoreInitFailed_) {
        if (!terrainPageStore_) {
            auto store = std::make_unique<TerrainPageStore>();
            TerrainPageStore::Config pageStoreConfig;
            // 合成下 worker(真机 compose 单帧尖刺 33-37ms 的归属定案)。
            pageStoreConfig.composeWorkers = &AsyncSystem::pool();
            if (store->initialize(device_, pageStoreConfig)) {
                terrainPageStore_ = std::move(store);
                // surface 重建会重建页存储 → 叠画钩子必须重新挂上,否则矢量
                // 在重建后静默消失(且无任何报错)。
                scene_->setTerrainPageStore(terrainPageStore_.get());
            } else {
                terrainPageStoreInitFailed_ = true;
            }
        }
        // 渲染线程驱动:目标锁定后 kick 异步影像 fetch + 排空已到达影像灌 layer。
        if (terrainPageStore_) {
            const double pageStoreStartMs = perf::nowMs();
            // 北极星 SVT B2a 门②:选择完成(FrameState + tilePlan 已填)后、tick 前,
            // 跑「屏幕可见影像页 determination」并插桩(纯读 + log,不碰池/fetch/render)。
            // 主相机视图 + 本帧可见瓦片 + 影像 provider 三者齐备才跑;否则跳过。
            const FrameState& frameState = scene_->frameState();
            Tileset* tileset = scene_->tileset();
            if (!frameState.selectorViews.empty() && tileset != nullptr) {
                // C-1:把**整个有序** overlay 列表交给页存储(与 mappedRaster 同序
                // 合成)。此前只传 overlays.front(),靠后的 overlay 在页存储路径上
                // 被静默丢弃 —— 两条合成路径语义不一致正是矢量层贴地失效的根。
                const std::vector<ActivatedRasterOverlay*>& overlays =
                    tileset->rasterOverlays();
                pageProvidersScratch_.clear();
                pageProvidersScratch_.reserve(overlays.size());
                for (ActivatedRasterOverlay* overlay : overlays) {
                    if (overlay == nullptr) continue;
                    if (RasterOverlayTileProvider* p = overlay->getTileProvider()) {
                        pageProvidersScratch_.push_back(p);
                    }
                }
                if (!pageProvidersScratch_.empty()) {
                    terrainPageStore_->updateVisiblePages(
                        frameState.selectorViews.front(),
                        tileset->tilePlan().tilesToRenderThisFrame,
                        pageProvidersScratch_,
                        tileset->maximumScreenSpaceError());
                }
            }
            terrainPageStore_->tick();
            // 这段跑在 update 与 begin 之间,不属于头行任何既有分段 ——
            // 不单独计时的话,页存储的合成/上传/叠画成本在慢帧归因里
            // 表现为「总量减分段的无名差值」(z9-10 pan 实测差值 ~170ms)。
            pageStoreMs = perf::nowMs() - pageStoreStartMs;
        }
    }
    if (scene_->shouldHoldPresentationFrame()) {
        lastFramePresented_ = false;
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
        if (gpuPassTimingEnabled_) {
            device_->beginGpuFrame(scene_->frameState().frameId);
        }
        // 离屏后处理(flag ON 且资源可用时):场景 pass 的目标换成离屏
        // FBO,场景后追加全屏后处理 pass 上屏;任何一环失败都回落直绘。
        // 优先级:AerialFog > FXAA > passthrough 调试直通。
        // kEnableHdrPipeline(T2):场景画进线性 HDR 靶,Tonemap 作**强制终端**
        // (最高优先级——场景一旦是 HDR,终端必须 tonemap+encode;与 FXAA/fog
        // 的组合是后续,切片先单 Tonemap)。默认关 → 走原逻辑,零变化。
        const bool wantOffscreen =
            kEnableHdrPipeline || aerialFogEnabled_ || fxaaEnabled_ ||
            offscreenPassthroughEnabled_;
        // HDR 开时 tonemap 是强制终端;fog 也开则走合并终端(B0,fog 混在
        // tonemap 前的线性域,消地平线硬切),否则单 tonemap。HDR 关时保持
        // 原互斥优先级 AerialFog > FXAA > passthrough。
        const OffscreenPostProcess::Effect wantEffect =
            kEnableHdrPipeline
                ? (aerialFogEnabled_
                       ? OffscreenPostProcess::Effect::AerialFogTonemap
                       : OffscreenPostProcess::Effect::Tonemap)
            : aerialFogEnabled_ ? OffscreenPostProcess::Effect::AerialFog
            : fxaaEnabled_      ? OffscreenPostProcess::Effect::Fxaa
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
        // 场景 pass 区间在 beginPass **之前**开:这样 clear(MSAA 下是 4× 采样的
        // 清屏,本身就是笔实打实的带宽)落在 "pass.scene.clear" 名下,而不是被
        // 摊进第一个命令桶(那会让 env 或 terrain 平白背上一笔清屏账)。
        // submit() 里第一个命令桶开始时,本区间自动结束——区间平铺不嵌套。
        if (gpuPassTimingEnabled_) {
            device_->beginGpuRegion("pass.scene.clear");
        }
        offscreenPassActive_ =
            offscreenTarget && device_->beginPass(offscreenTarget);
        if (!offscreenPassActive_) {
            device_->beginPass(nullptr);
        }
        // T2:把场景 pass 的目标交给 Scene —— 地形深度 prepass 会临时切走
        // pass,跑完必须切回这里(离屏失败时是 nullptr = 直绘主 pass)。
        scene_->setSceneRenderTarget(
            offscreenPassActive_ ? offscreenTarget : nullptr,
            surfaceWidthPixels_, surfaceHeightPixels_);
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
                // 后处理是单命令全屏 pass,没有可分的桶 → 整段一个区间。
                if (gpuPassTimingEnabled_) {
                    device_->beginGpuRegion("pass.postProcess", false);
                }
                device_->submit({offscreenPostProcess_->buildCommand(params)});
                if (gpuPassTimingEnabled_) {
                    device_->endGpuRegion();
                }
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

    lastFramePresented_ = scenePresented;
    // 黑块探针(漏底/黑块诊断):swap 前逐帧降采样回读,统计近黑像素占比。
    // 为什么必须逐帧:截图/录屏抽样会漏帧,"抽查没看到"证明不了"没有"
    // (漏底案实测:HoleQual drop=0 的帧仍可能画出黑块 —— 机制信号量的是
    // "选中未建条",黑块可能来自"画了但纹理黑",两者正交,只有像素级
    // 逐帧信号能兜底)。近黑判据 RGB 全 ≤8:真实影像的山影是深灰绿,
    // 大块纯黑只会来自未就绪纹理/清屏底色。含同步回读(~1-2ms/帧),
    // 仅诊断会话开启。
    if (blackFrameProbeEnabled_ && device_) {
        const int grid = device_->captureFrameSample(blackProbeScratch_);
        if (grid <= 0) {
            // 回读不可用时必须显式报告并关闭 —— "打开了但没数"与"没黑块"
            // 读数相同,静默降级会把瞎掉的守卫伪装成绿色(ShadowVerify 踩过)。
            blackFrameProbeEnabled_ = false;
            platformLog(LogLevel::Warning, "BlackProbe",
                        "disabled: 帧采样不可用(后端不支持或回读失败)");
        } else {
            const size_t n = blackProbeScratch_.size();
            int dark = 0;
            for (size_t i = 0; i + 3 < n; i += 4) {
                if (blackProbeScratch_[i] <= 8 &&
                    blackProbeScratch_[i + 1] <= 8 &&
                    blackProbeScratch_[i + 2] <= 8) {
                    ++dark;
                }
            }
            const double frac =
                static_cast<double>(dark) /
                static_cast<double>(grid * grid);
            ++blackProbeFrames_;
            if (frac > blackProbeWorstFrac_) blackProbeWorstFrac_ = frac;
            // 0.5% ≈ 256² 里 328 像素,一块可见瓦片的量级;低于它的零星
            // 黑点(文字描边/UI)不报。
            if (frac >= 0.005) {
                ++blackProbeHits_;
                platformLog(LogLevel::Warning, "BlackProbe",
                            "frame=%llu darkFrac=%.4f dark=%d grid=%d",
                            static_cast<unsigned long long>(
                                scene_->frameState().frameId),
                            frac, dark, grid);
            }
            // 心跳:证明探针活着。"无告警"必须能与"没在跑"区分开。
            if ((blackProbeFrames_ % 300) == 1) {
                platformLog(LogLevel::Info, "BlackProbe",
                            "alive frames=%llu hits=%llu worst=%.4f",
                            static_cast<unsigned long long>(blackProbeFrames_),
                            static_cast<unsigned long long>(blackProbeHits_),
                            blackProbeWorstFrac_);
            }
        }
    }
    // 影子渲染自检:在 swap **之前**取帧指纹(见 setShadowVerifyEnabled)。
    // 只在自检窗口里跑 —— 这一步含同步回读,常开会污染所有帧时读数。
    if (shadowVerifyFramesLeft_ > 0 && device_) {
        const int grid = device_->captureFrameSample(shadowVerifyScratch_);
        --shadowVerifyFramesLeft_;
        if (grid <= 0) {
            // 后端不支持/回读失败。**不能当成"没变化"** —— 那会把一个没在
            // 工作的守卫伪装成绿色(实测踩过:MSAA 格式不匹配导致 blit 静默
            // 失败,故意把画面改花仍报 0 差异)。
            shadowVerifyFramesLeft_ = 0;
            shadowVerifyDoneThisIdle_ = true;
            platformLog(LogLevel::Info, "ShadowVerify",
                        "skipped: 帧采样不可用(后端不支持或回读失败)");
        } else if (shadowVerifyBaseline_.empty()) {
            shadowVerifyBaseline_ = shadowVerifyScratch_;
        } else if (shadowVerifyBaseline_.size() ==
                   shadowVerifyScratch_.size()) {
            // 报**变化量**而不是"变没变":1 个最低位的舍入噪声与"一块瓦片
            // 出现了"在二值读数上完全一样,而这两者的处置天差地别。
            // 噪声门限:实测健康态(时钟已冻、抖动是屏幕位置函数)仍有
            // 47/65536 像素出现 **delta=1** 的差异 —— MSAA resolve 与线性
            // 降采样的量化舍入不是逐帧位级确定的。不设门限的话守卫在完全
            // 正常的画面上也 19/19 全红,而"一直报警"比没有守卫更糟:人会
            // 学会无视它。代价:全屏幅度 ≤1 LSB 的变化抓不到 —— 那种变化
            // 肉眼同样看不见,不是这个守卫要防的东西。
            constexpr int kNoiseDelta = 1;
            int diffPixels = 0;
            int maxDelta = 0;
            const size_t n = shadowVerifyScratch_.size();
            for (size_t i = 0; i + 3 < n; i += 4) {
                int pixelDelta = 0;
                for (size_t c = 0; c < 3; ++c) {
                    const int d = std::abs(
                        static_cast<int>(shadowVerifyScratch_[i + c]) -
                        static_cast<int>(shadowVerifyBaseline_[i + c]));
                    if (d > pixelDelta) pixelDelta = d;
                }
                if (pixelDelta > kNoiseDelta) {
                    ++diffPixels;
                    if (pixelDelta > maxDelta) maxDelta = pixelDelta;
                }
            }
            if (diffPixels > 0) {
                ++shadowVerifyMismatches_;
                if (diffPixels > shadowVerifyWorstPixels_) {
                    shadowVerifyWorstPixels_ = diffPixels;
                }
                if (maxDelta > shadowVerifyWorstDelta_) {
                    shadowVerifyWorstDelta_ = maxDelta;
                }
                shadowVerifyBaseline_ = shadowVerifyScratch_;
            }
        }
        if (shadowVerifyFramesLeft_ == 0 && !shadowVerifyDoneThisIdle_) {
            shadowVerifyDoneThisIdle_ = true;
            const int total = grid > 0 ? grid * grid : 1;
            if (shadowVerifyMismatches_ > 0) {
                platformLog(LogLevel::Error, "ShadowVerify",
                            "判定 idle 之后画面仍在变:changedFrames=%d "
                            "worstPixels=%d/%d(%.2f%%) worstDelta=%d "
                            "—— 有异步产物落地却没人置脏位,或有逐帧时变项没关",
                            shadowVerifyMismatches_, shadowVerifyWorstPixels_,
                            total,
                            100.0 * shadowVerifyWorstPixels_ / total,
                            shadowVerifyWorstDelta_);
            } else {
                // 干净也要出行:没有这一行,"自检通过"与"自检根本没跑"
                // 在日志里读数相同 —— 那正是本守卫要治的病。
                platformLog(LogLevel::Info, "ShadowVerify",
                            "窗口干净(%d 帧无变化,门限 delta>%d)",
                            kShadowVerifySampleFrames, 1);
            }
            shadowVerifyBaseline_.clear();
            shadowVerifyMismatches_ = 0;
            shadowVerifyWorstPixels_ = 0;
            shadowVerifyWorstDelta_ = 0;
        }
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
        "begin=%.2f update=%.2f pageStore=%.2f render=%.2f submit=%.2f end=%.2f draw=%d tiles=%d hold=%d%s%s%s",
        diag.engineBeginFrameMs,
        diag.sceneUpdateMs,
        pageStoreMs,
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

    logGpuPassTiming();

    // 环境快照 runtime 段:每帧喂帧耗时,内部满周期(600 帧)才打一行。
    // 报的是**执行条件**而非性能数字本身——落在哪个核、缓存冷热、这一窗口的
    // 工作量(tiles/draw)。跨运行比较前先看这行是否可比,详见 EnvSnapshot.h。
    {
        envsnap::RuntimeFields env;
        env.cpu = RenderThreadPlacement::currentCpu();
        env.visibleTiles = diag.visibleTiles;
        env.drawCalls = diag.drawCalls;
        const HttpCache::Stats http = HttpCache::shared().stats();
        env.httpLookups = http.lookups;
        env.httpHitPercent = http.lookups > 0
            ? static_cast<int>((http.hits * 100) / http.lookups)
            : 0;
        envsnap::tickRuntime(scene_->frameState().frameId,
                             diag.engineFrameCpuMs,
                             env);
    }
    // 层间契约帧末汇总:全绿不打(稳态零日志量),出现即定位到具体的那条边。
    contracts::logFrameSummary(scene_->frameState().frameId);
    // 覆盖行与 EnvSnap 同周期。这条**总是**打:全绿时它是唯一能证明契约还活着
    // 的东西。某条边 coverage 长期为 0 = 判定点没跑到 = 那条契约等于不存在。
    if (const uint64_t contractFrameId = scene_->frameState().frameId;
        contractFrameId > 0 && contractFrameId % 600 == 0) {
        contracts::logCoverage(contractFrameId);
        // E6:高度采样统计并入本窗口。命中率进 Policy;命中档位直方图只出
        // Info 行(引擎内无可辩护的"目标档",原始分布留给分析期对照场景),
        // 无查询窗口(高空早退)静默——与"分母 0 不参与判定"同理。
        if (Tileset* heightTileset = scene_->tileset()) {
            const TerrainHeightService::SampleStats stats =
                heightTileset->heightService().takeSampleStats();
            if (stats.total() > 0) {
                policy::observe(
                    policy::Id::HeightSampleCoverage,
                    static_cast<int>(
                        std::min<std::uint64_t>(stats.hits, INT_MAX)),
                    static_cast<int>(
                        std::min<std::uint64_t>(stats.total(), INT_MAX)));
                char hist[192];
                int off = std::snprintf(
                    hist, sizeof(hist),
                    "f=%llu hit=%llu miss=%llu z:",
                    static_cast<unsigned long long>(contractFrameId),
                    static_cast<unsigned long long>(stats.hits),
                    static_cast<unsigned long long>(stats.misses));
                for (std::size_t z = 0; z < stats.zoomHits.size(); ++z) {
                    if (stats.zoomHits[z] == 0 ||
                        off >= static_cast<int>(sizeof(hist)) - 16) {
                        continue;
                    }
                    off += std::snprintf(
                        hist + off, sizeof(hist) - static_cast<size_t>(off),
                        " %zu=%u", z, stats.zoomHits[z]);
                }
                platformLog(LogLevel::Info, "HeightSamp", "%s", hist);
            }
        }
        // 策略生效率报表:与契约 coverage 同周期。契约答"单点是否成立",策略答
        // "整体比率是否落在预期区间" —— 合批空转那次错的是后者,前者全绿。
        policy::logReport(contractFrameId);
    }
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

CameraSystem& Engine::cameraSystem() {
    return scene_->cameraSystem();
}

// ---- 矢量图层 ----

void Engine::addVectorLayer(std::unique_ptr<VectorLayer> layer) {
    scene_->addVectorLayer(std::move(layer));
}

std::unique_ptr<VectorLayer> Engine::removeVectorLayer(const std::string& layerId) {
    return scene_->removeVectorLayer(layerId);
}

void Engine::addFeatureRenderLayer(std::unique_ptr<FeatureRenderLayer> layer) {
    scene_->addFeatureRenderLayer(std::move(layer));
}

std::unique_ptr<FeatureRenderLayer> Engine::removeFeatureRenderLayer(
    const std::string& layerId) {
    return scene_->removeFeatureRenderLayer(layerId);
}

bool Engine::setLabelFontData(std::vector<uint8_t> fontData) {
    return scene_->setLabelFontData(std::move(fontData));
}

bool Engine::addIconImage(const std::string& name,
                          int width,
                          int height,
                          const std::vector<uint8_t>& rgba) {
    return scene_->addIconImage(name, width, height, rgba);
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

void Engine::setSunsetTerrainTint(float warmth, float shadowScale) {
    scene_->setSunsetTerrainTint(warmth, shadowScale);
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

double Engine::cameraHeadingRadians() const {
    return scene_ ? scene_->cameraSystem().headingRadians() : 0.0;
}

void Engine::resetNorthUp() {
    if (scene_) scene_->cameraSystem().resetNorthUp();
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
