#pragma once

#include <limits>

#include "Diagnostics.h"
#include "FrameState.h"
#include "PolarCapRenderer.h"
#include "../renderer/RenderCommand.h"
#include "../renderer/TerrainDepthPrepass.h"
#include "../renderer/TerrainInstanceBatcher.h"

#include <functional>
#include <memory>
#include <vector>

namespace earth_engine {

class AtmosphereBackgroundPass;
class FeatureRenderLayer;
class Renderer;
class RenderDevice;
class SkyBox;
class SkyGradient;
class Tileset;
class VectorLayer;

/// Lightweight render pass coordinator for Scene's main color pipeline.
///
/// Scene owns long-lived state and updates it; this class turns the current
/// frame state into ordered render commands, validates them, aggregates render
/// diagnostics, and submits the frame.
class SceneRenderPipeline {
public:
    struct Result {
        Diagnostics diagnostics;
        bool presentable = true;
    };

    struct Context {
        FrameState& frameState;
        Diagnostics diagnostics;
        Renderer& renderer;
        RenderCommandList& commands;
        SkyBox* skyBox = nullptr;
        AtmosphereBackgroundPass* atmospherePass = nullptr;
        SkyGradient* skyGradient = nullptr;
        Tileset* terrainTileset = nullptr;
        Tileset* pendingTerrainTileset = nullptr;
        const std::vector<std::unique_ptr<Tileset>>& additionalTilesets;
        std::vector<std::unique_ptr<VectorLayer>>& vectorLayers;
        // FeatureStore 渲染桥接层(矢量 P1),与 vectorLayers 平行的新路径
        std::vector<std::unique_ptr<FeatureRenderLayer>>& featureRenderLayers;
        std::function<void()> beforeSubmit;
        RenderDevice* renderDevice = nullptr;
        // T2 地形深度 prepass 需要在 submit 前插一个 pass,插完必须把场景
        // pass 重新 begin 回来 —— 故要知道场景 pass 的目标(离屏 FBO,直绘
        // 时为 nullptr)。此刻场景 FBO 上尚未画任何东西,重新 begin 带来的
        // 二次 clear 无害。
        Framebuffer* sceneTarget = nullptr;
        int surfaceWidthPixels = 0;
        int surfaceHeightPixels = 0;
    };

    Result render(Context context);

private:
    void reserveCommands(Context& context) const;
    void buildSkyCommands(Context& context, double& skyMs) const;
    void buildAtmosphereCommands(Context& context, double& atmosphereMs) const;
    void buildLayerCommands(Context& context,
                            double& layerCommandsMs,
                            double& vectorCommandsMs) const;
    void assembleTerrainBatches(Context& context, double& batchMs) const;
    void applyMvpUniforms(Context& context, double& mvpUniformsMs) const;
    void sortAndValidate(Context& context,
                         double& sortMs,
                         double& surfaceDiagnosticsMs,
                         double& validateMs) const;
    // T2:在主 submit 之前跑地形深度 prepass,跑完把场景 pass 重新 begin
    // 回来。prepass 不可用 / 本帧无真实地形时整体 no-op。
    void prepareTerrainOcclusion(Context& context) const;
    void runTerrainDepthPrepass(Context& context) const;
    void aggregateDiagnostics(Context& context, double& diagnosticsMs) const;
    bool shouldHoldPresentationAfterCommandBuild(const Context& context) const;
    // presentable 透传给契约判定(hold/跳帧没有需要保活的提交,不参与判定)。
    void releaseRenderReferences(Context& context, bool presentable) const;

    mutable int lastPrimaryCurrentEntryCount_ = -1;
    mutable int lastPrimaryPendingEntryCount_ = -1;
    // 本帧喂给矢量层的贴地高度范围(米),仅供头行诊断读取。
    mutable double lastClampMinHeight_ = 0.0;
    mutable double lastClampMaxHeight_ = 0.0;
    mutable bool lastClampRangeApplied_ = false;
    PolarCapRenderer polarCap_;
    mutable TerrainInstanceBatcher terrainBatcher_;
    // BatchDet 判因行的节流计数(独立于帧号,与 PageDet 同模式)。
    mutable uint32_t batchDetFrameCounter_ = 0;
    // 本帧 submit 已完成的帧号,供 SubmitBeforeReleaseRefs 契约比对。
    mutable uint64_t submitDoneFrameId_ = std::numeric_limits<uint64_t>::max();
    // T2:地形深度 prepass。首帧惰性 initialize;不可用时全程 no-op。
    mutable TerrainDepthPrepass terrainDepthPrepass_;
    mutable bool terrainDepthPrepassInitAttempted_ = false;
};

} // namespace earth_engine
