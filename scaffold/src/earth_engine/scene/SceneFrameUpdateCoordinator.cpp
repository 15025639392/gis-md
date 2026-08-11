#include "SceneFrameUpdateCoordinator.h"

#include "Diagnostics.h"
#include "SceneFrameDiagnostics.h"
#include "SceneFrameStateBuilder.h"
#include "SceneTilesetCoordinator.h"
#include "Camera.h"
#include "../camera/CameraSystem.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../debug/PerfTimer.h"
#include "../debug/PlatformLog.h"

#include <algorithm>
#include <cstdio>

namespace earth_engine {

void SceneFrameUpdateCoordinator::update(
    const SceneFrameUpdateInput& input) {
    const double updateStartMs = perf::nowMs();

    const double t0 = perf::nowMs();
    SceneFrameDiagnostics::resetPerFrame(input.diagnostics);
    SceneFrameDiagnostics::updateFrameRate(
        input.diagnostics,
        input.deltaSeconds);
    const double t_reset = perf::nowMs();

    double cameraUpdateMs = 0.0;
    if (input.cameraSystem) {
        const double startMs = perf::nowMs();
        input.cameraSystem->update(input.deltaSeconds);
        cameraUpdateMs = perf::nowMs() - startMs;
    }
    const double t_cam = perf::nowMs();

    // Dynamic near plane: reverse-Z with a fixed near=150/far=1e12 crams every
    // real distance into a tiny, poorly-conditioned z_ndc range (~2e-5) at
    // altitude. The precision loss is UPSTREAM of the depth buffer — the float32
    // clip-space z computed in the vertex shader can't separate ~1 m depth
    // differences when z_ndc lives at 2e-5 — so a bigger depth buffer does NOT
    // help; only tightening the near plane (moving z_ndc into a well-conditioned
    // range ~0.5) does. Far stays as configured (1e12).
    //
    // 距离源 = CameraSystem::groundState().nearestGeometryMeters(近场探针
    // 采样最小距离 ∧ 盘外墙下界):高空构造性退化为 椭球高−9000(与旧椭球
    // nadir 公式相对差 <1%,z-fighting 零回归);低空/掠视按真实最近坡体收紧
    // ——旧公式按椭球 nadir 不扣地形高,高原上空 500m 时 near=2750m,前方坡体
    // 全被近平面切掉("看到山内部"的根因)。下限/比例常量与碰撞净空的耦合
    // 契约见 CameraSystem 头(static_assert 锁定)。groundState 未解算过
    // (headless/无控制器)退回旧公式与旧下限。
    if (input.camera) {
        double nearPlane;
        if (input.cameraSystem &&
            input.cameraSystem->groundState().valid) {
            nearPlane = std::max(
                CameraSystem::kNearFloorMeters,
                CameraSystem::kNearSafetyRatio *
                    input.cameraSystem->groundState()
                        .nearestGeometryMeters);
        } else {
            const double nadirDistance =
                input.camera->position().length() -
                Ellipsoid::WGS84().maximumRadius();
            nearPlane = std::max(150.0, nadirDistance * 0.5);
        }
        input.camera->setPerspective(
            input.camera->verticalFovRadians(),
            nearPlane,
            input.camera->farPlaneMeters());
    }

    // 飞行契约必须在 FrameState 构建**之前**取,且在 cameraSystem->update() 之后
    // ——瓦片系统本帧就要据它决定关不关 cullRequestsWhileMoving(见 FrameState
    // 里那两个字段的说明:漏了就是"飞到目的地画面是空的")。
    if (input.cameraSystem) {
        input.frameState.cameraFlightActive =
            input.cameraSystem->cameraFlightActive();
        input.frameState.cameraFlightProgress =
            input.cameraSystem->cameraFlightProgress();
    } else {
        input.frameState.cameraFlightActive = false;
        input.frameState.cameraFlightProgress = 0.0;
    }

    input.elapsedTime += input.deltaSeconds;
    SceneFrameStateBuildResult frameStateResult =
        SceneFrameStateBuilder::build(SceneFrameStateBuildInput{
            input.frameState,
            input.camera,
            ++input.frameId,
            input.elapsedTime,
            input.deltaSeconds,
            input.hasSelectorViewOverride,
            input.selectorViewOverride,
            input.hasInteractionFocus,
            input.interactionFocusDirection,
            input.interactionFocusTimeSeconds,
            input.timeController,
            input.skyGradient});
    const double t_fsb = perf::nowMs();
    input.diagnostics.cameraUpdateMs = cameraUpdateMs;
    input.diagnostics.environmentUpdateMs =
        frameStateResult.environmentUpdateMs;

    SceneTilesetUpdateResult tilesetUpdateResult =
        input.tilesets.update(input.frameState, input.pPrepRenderer);
    const double t_tsu = perf::nowMs();
    input.diagnostics.terrainUpdateMs = tilesetUpdateResult.terrainUpdateMs;
    input.diagnostics.contentTilesetUpdateMs =
        tilesetUpdateResult.contentTilesetUpdateMs;

    const double totalMs = perf::nowMs() - updateStartMs;
    char detail[192];
    std::snprintf(detail, sizeof(detail),
        "camera=%.2f env=%.2f basemap=%.2f terrain=%.2f content=%.2f",
        input.diagnostics.cameraUpdateMs,
        input.diagnostics.environmentUpdateMs,
        input.diagnostics.basemapStackUpdateMs,
        input.diagnostics.terrainUpdateMs,
        input.diagnostics.contentTilesetUpdateMs);
    perf::logTiming(input.frameState.frameId,
                    "Scene.update.total",
                    totalMs,
                    detail);
    // Log per-section timing every frame if total > 30ms
    if (totalMs > 30.0) {
        platformLog(LogLevel::Info, "EarthPerf",
            "Scene.breakdown: reset=%.2f cam=%.2f fsb=%.2f tsu=%.2f total=%.2f",
            t_reset - t0, t_cam - t_reset, t_fsb - t_cam, t_tsu - t_fsb, totalMs);
    }
}

} // namespace earth_engine
