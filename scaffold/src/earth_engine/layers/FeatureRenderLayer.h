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
    /// 桶空/全退化 → 从 buckets_ 移除。
    void rebuildBucket(BucketKey key);

    std::string layerId_;
    bool visible_ = true;
    RenderDevice* renderDevice_ = nullptr;
    const Ellipsoid& ellipsoid_;
    FeatureRenderStyle style_;
    FeatureStore store_;
    std::unordered_map<BucketKey, BucketGpu> buckets_;
};

} // namespace earth_engine
