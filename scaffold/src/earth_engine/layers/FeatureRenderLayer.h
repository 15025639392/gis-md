#pragma once

#include "../data/FeatureStore.h"
#include "../renderer/RenderCommand.h"
#include "../core/math/Vec3.h"

#include <array>
#include <memory>
#include <string>
#include <unordered_map>

namespace earth_engine {

class Ellipsoid;
class RenderDevice;
class Renderer;
struct FrameState;

/// 要素拾取结果(编辑基础接口,设计原则见 P2 分层:引擎只出查询,
/// 选中态/编辑会话归应用层)。
struct FeaturePickResult {
    enum class Part { None, Vertex, Edge, Fill };

    FeatureId featureId = kInvalidFeatureId;
    Part part = Part::None;
    int ringIndex = -1;
    /// Part::Vertex = 命中顶点序号;Part::Edge = 边起点序号
    /// (边 = [v, v+1],polygon 环的闭合末边 = [n-1, 0])。
    int vertexIndex = -1;
    /// Vertex/Edge:屏幕像素距离;Fill:0。
    double distancePx = 0.0;
    /// Vertex = 该顶点存储坐标;Edge = 边上最近点(线性插值);
    /// Fill = 拾取射线与椭球交点。
    Cartographic position;

    bool isValid() const { return part != Part::None; }
};

/// FeatureRenderLayer 的图层级样式(矢量 P1:字面量样式子集,
/// data-driven 表达式属 P6)。
struct FeatureRenderStyle {
    std::array<float, 4> fillColor{0.25f, 0.55f, 0.95f, 0.35f};
    std::array<float, 4> lineColor{1.00f, 0.80f, 0.10f, 0.90f};
    float lineWidthPx = 4.0f;
    /// 顶点高程偏移(m)。贴地钳制(P3)落地前用于把要素抬离地表防 z-fight。
    double heightOffset = 0.0;
};

/// FeatureStore → GPU 的渲染桥接层(矢量数据系统 P1,设计 §6/§9)。
///
/// 持有权威 FeatureStore,按其空间分桶组织 GPU 几何:每个桶一份常驻
/// fill mesh(CDT 三角化)+ line mesh(ribbon,§6.2 屏幕挤出),编辑/导入
/// 只重镶脏桶。与旧 VectorLayer(GeoJSON 标注路径)平行,不共享代码。
///
/// 精度:桶内顶点存**相对桶原点**的 float(原点 = 桶首个 ECEF 顶点,double),
/// 每帧以 double 计算 mvp = viewProj · translate(origin) 后降 float —— 对齐
/// 地形 RTE 路径,消除 ECEF 直存 float 的米级抖动。
///
/// 线程契约:syncDirtyBuckets/buildRenderCommands 必须在渲染线程调用
/// (buffer 创建需 GL 上下文);FeatureStore 写入也须同线程(store 非线程安全)。
/// 镶嵌 worker 化留待 P2 编辑高频场景验证后决定。
///
/// P1 范围:Polygon fill + 外环 outline、LineString;Point 要素跳过(P5 符号)。
class FeatureRenderLayer {
public:
    FeatureRenderLayer(std::string layerId,
                       RenderDevice* renderDevice,
                       const Ellipsoid& ellipsoid);
    ~FeatureRenderLayer();

    FeatureRenderLayer(const FeatureRenderLayer&) = delete;
    FeatureRenderLayer& operator=(const FeatureRenderLayer&) = delete;

    const std::string& id() const { return layerId_; }
    void setVisible(bool v) { visible_ = v; }
    bool visible() const { return visible_; }

    FeatureStore& store() { return store_; }
    const FeatureStore& store() const { return store_; }

    void setStyle(const FeatureRenderStyle& s) { style_ = s; }
    const FeatureRenderStyle& style() const { return style_; }

    /// 消费 store 脏桶并重镶/上传(渲染线程)。返回重镶桶数(诊断)。
    /// buildRenderCommands 内部已调用;单独暴露供测试与诊断。
    int syncDirtyBuckets();

    /// 每帧生成 VectorFill/VectorLine 命令(渲染线程)。
    void buildRenderCommands(const FrameState& frameState,
                             Renderer& renderer,
                             RenderCommandList& commands);

    // ---- 编辑基础接口(引擎只出查询与预览通道,会话/undo/手柄归应用层) ----

    /// 屏幕点拾取:射线∩椭球定邻域 → R-tree 预筛 → 候选投影到屏幕比像素
    /// 距离。优先级 Vertex > Edge > Fill(容差内)。几何按渲染态(含
    /// style.heightOffset)投影;返回坐标是存储值(不含 offset)。
    /// Point 要素的顶点参与拾取(即使 P5 前不渲染),应用层可自行过滤。
    /// 渲染线程调用(读 store;与编辑写入同线程)。
    FeaturePickResult pick(const FrameState& frameState,
                           float screenXPx,
                           float screenYPx,
                           float tolerancePx) const;

    /// 编辑预览通道:拖拽高频期把要素从常驻桶摘出走瞬态路径,不进
    /// store、不触发全桶重镶。流程:begin(摘除+快照)→ 每次拖拽
    /// update(rings)→ 结束 end(调用方自行决定是否先 updateFeature
    /// commit)。同一时刻至多一个预览要素。渲染线程调用。
    bool beginEditPreview(FeatureId id);
    void updateEditPreview(std::vector<std::vector<Cartographic>> rings);
    void endEditPreview();
    FeatureId previewFeatureId() const { return previewFeatureId_; }

    /// 常驻 GPU 桶数(测试/诊断)。
    size_t gpuBucketCount() const { return buckets_.size(); }

private:
    /// 单桶常驻 GPU 几何。fill/line 任一可空(indexCount=0)。
    struct BucketGpu {
        Vec3 origin = Vec3::zero();        ///< ECEF double 原点
        std::unique_ptr<Buffer> fillVertexBuffer;
        std::unique_ptr<Buffer> fillIndexBuffer;
        int fillIndexCount = 0;
        std::unique_ptr<Buffer> lineVertexBuffer;
        std::unique_ptr<Buffer> lineIndexBuffer;
        int lineIndexCount = 0;
    };

    /// 重镶单桶:镶嵌桶内全部要素 → 减原点转 float → 建 buffer。
    /// 桶空/全退化 → 从 buckets_ 移除。预览摘除中的要素跳过。
    void rebuildBucket(BucketKey key);

    /// 镶嵌单要素几何并追加进 CPU 侧数组(rebuildBucket 与预览路径共用)。
    void tessellateFeatureInto(const Feature& feature,
                               Vec3& origin,
                               bool& hasOrigin,
                               std::vector<float>& fillVerts,
                               std::vector<uint32_t>& fillIndices,
                               std::vector<float>& lineVerts,
                               std::vector<uint32_t>& lineIndices) const;

    /// CPU 数组 → BucketGpu(buffer 创建;全空返回 false)。
    bool uploadBucketGpu(const Vec3& origin,
                         const std::vector<float>& fillVerts,
                         const std::vector<uint32_t>& fillIndices,
                         const std::vector<float>& lineVerts,
                         const std::vector<uint32_t>& lineIndices,
                         BucketGpu& out) const;

    /// 生成一对 fill/line 命令追加进 commands(常驻桶与预览路径共用)。
    void appendBucketCommands(const BucketGpu& gpu,
                              const FrameState& frameState,
                              Renderer& renderer,
                              RenderCommandList& commands) const;

    std::string layerId_;
    bool visible_ = true;
    RenderDevice* renderDevice_ = nullptr;
    const Ellipsoid& ellipsoid_;
    FeatureRenderStyle style_;
    FeatureStore store_;
    std::unordered_map<BucketKey, BucketGpu> buckets_;

    // ---- 编辑预览态 ----
    FeatureId previewFeatureId_ = kInvalidFeatureId;
    GeometryType previewType_ = GeometryType::Point;
    std::vector<std::vector<Cartographic>> previewRings_;
    bool previewDirty_ = false;
    BucketGpu previewGpu_;
    bool previewGpuValid_ = false;
};

} // namespace earth_engine
