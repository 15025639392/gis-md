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

    /// 一条命令是否算「地形表面命令」。presentation hold 判据的原子谓词。
    ///
    /// 提成公开纯函数是为了**可被单独证伪**:它错一次的代价是整屏永久定格且
    /// 零报错(合批把地形换成 GltfPrimitiveInstanced 后,只认 GltfPrimitive 的
    /// 旧版本在"可见地形全部合批"时数到 0),而这个错误在任何截图、任何计数
    /// 日志里都看不出来 —— 只能靠对着命令形态直接断言。
    static bool isTerrainSurfaceCommandForTest(const RenderCommand& command);

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
    /// 命令构建后那条 hold 闸的活性兜底(与 Scene 侧同款上限)。连续扣住超过
    /// 此数即无条件放行 —— 没有它,判据一旦出错就是整屏永久定格且零报错。
    static constexpr int kMaximumPostBuildHeldFrames = 60;
    mutable int consecutivePostBuildHeldFrames_ = 0;
    // presentable 透传给契约判定(hold/跳帧没有需要保活的提交,不参与判定)。
    void releaseRenderReferences(Context& context, bool presentable) const;

    mutable int lastPrimaryCurrentEntryCount_ = -1;
    mutable int lastPrimaryPendingEntryCount_ = -1;
    // 本帧喂给矢量层的贴地高度范围(米),仅供头行诊断读取。
    mutable double lastClampMinHeight_ = 0.0;
    mutable double lastClampMaxHeight_ = 0.0;
    mutable bool lastClampRangeApplied_ = false;
    /// 参与贴地高度汇总的瓦片数:tight = 瓦片自己测得的真高度,ancestor =
    /// 包围体是占位值、改由高度服务向上找到的祖先高度图供的实测值,
    /// loose = 连祖先都没有、只能吃占位 -1000/9000。
    /// 只有一对 clampH 数时,「没有 loose 瓦片」与「loose 判据失效」读数相同;
    /// t 与 a 再分开,才看得出这一帧的范围是自产的还是靠祖先兜的。
    mutable int lastClampTightTiles_ = 0;
    mutable int lastClampAncestorTiles_ = 0;
    mutable int lastClampLooseTiles_ = 0;
    /// 高度服务索引里的瓦片数。a=0 有两个完全不同的病因:索引本身是空的,
    /// 还是索引有货但祖先链没覆盖到 —— 少了这个数,两者读数相同。
    mutable int lastHeightIndexTiles_ = 0;
    mutable int lastHeightIrregularTiles_ = 0;
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
