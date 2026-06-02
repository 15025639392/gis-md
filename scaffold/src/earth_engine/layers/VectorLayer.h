#pragma once

#include "../data/GeoJsonParser.h"
#include "../style/OverlayStyle.h"
#include "../renderer/RenderCommand.h"
#include "../core/math/Vec3.h"

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

namespace earth_engine {

class RenderDevice;
class Renderer;
struct FrameState;
class Camera;

/// 矢量图层。
///
/// 管理 GeoJSON feature 集合、样式和渲染。
/// 每个 feature 独立 tessellate 为 ECEF 顶点，通过 RenderCommand 提交。
///
/// 交互状态（hover/selected）通过 setFeatureState() 设置，
/// 不重建 geometry，仅通过 uniform 切换样式。
class VectorLayer {
public:
    /// @param layerId 图层唯一标识
    /// @param features GeoJSON 解析后的 feature 列表（所有权转移）
    /// @param style 叠加样式
    /// @param renderDevice 用于创建 GPU 资源
    VectorLayer(std::string layerId,
                std::vector<GeoFeature> features,
                OverlayStyle style,
                RenderDevice* renderDevice);
    ~VectorLayer();

    VectorLayer(const VectorLayer&) = delete;
    VectorLayer& operator=(const VectorLayer&) = delete;

    // ---- 基本信息 ----

    const std::string& id() const { return layerId_; }
    void setVisible(bool v) { visible_ = v; }
    bool visible() const { return visible_; }
    void setOpacity(float o);

    // ---- 样式 ----

    const OverlayStyle& style() const { return style_; }
    void setStyle(const OverlayStyle& s);

    /// 设置 feature 交互状态。
    /// 不重建 geometry，仅在下帧渲染时切换交互样式覆盖。
    void setFeatureState(const std::string& featureId,
                         const std::string& state);

    /// 清除所有 feature 的交互状态
    void clearAllStates();

    /// 获取 feature 当前状态
    std::string featureState(const std::string& featureId) const;

    // ---- 数据 ----

    const std::vector<GeoFeature>& features() const { return features_; }
    size_t featureCount() const { return features_.size(); }

    /// 按 feature ID 查找（O(n)，缓存 feature count 小时可接受）
    const GeoFeature* findFeature(const std::string& featureId) const;

    // ---- 渲染 ----

    /// 初始化 GPU 资源（shader、共享几何）
    bool initialize(RenderDevice* device);

    /// 每帧生成 RenderCommand
    void buildRenderCommands(const FrameState& frameState,
                             Renderer& renderer,
                             RenderCommandList& commands);

    /// 释放 GPU 资源
    void dispose();

private:
    /// 单个 feature 的 tessellate 结果（GPU 就绪）
    struct TessellateResult {
        std::vector<float> vertices;   // interleaved: xyz (ECEF meters)
        std::vector<uint32_t> indices;
        Vec3 centroid;                 // ECEF 中心（用于 billboard 点）
        size_t vertexCount = 0;
        size_t indexCount = 0;
    };

    /// tessellate 单个 feature
    TessellateResult tessellateFeature(const GeoFeature& feature) const;

    /// tessellate 点
    static TessellateResult tessellatePoint(const GeoFeature& feature);
    /// tessellate 线（沿椭球面细分）
    static TessellateResult tessellateLine(const GeoFeature& feature);
    /// tessellate 面（ear-clipping 三角化）
    static TessellateResult tessellatePolygon(const GeoFeature& feature);

    /// 根据 feature 状态获取当前样式覆盖
    InteractionStyleOverride resolveStyleOverride(
        const std::string& featureId) const;

    /// 获取 feature 当前渲染颜色
    Color resolveColor(const GeoFeature& feature,
                       const InteractionStyleOverride& override) const;

    std::string layerId_;
    bool visible_ = true;
    std::vector<GeoFeature> features_;
    OverlayStyle style_;

    // 交互状态表
    std::unordered_map<std::string, std::string> featureStates_;

    // GPU 资源
    RenderDevice* renderDevice_ = nullptr;
    bool initialized_ = false;

    // 缓存 tessellate 结果
    mutable std::unordered_map<std::string, TessellateResult> tessCache_;

    // 每帧动态 buffer（生命周期覆盖 buildRenderCommands → submit）
    mutable std::vector<std::unique_ptr<Buffer>> frameBuffers_;
};

} // namespace earth_engine
