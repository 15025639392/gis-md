#pragma once

#include "../data/FeatureStore.h"
#include "../data/StyleExpression.h"
#include "../renderer/RenderCommand.h"
#include "../renderer/SymbolShape.h"
#include "../core/math/Vec3.h"
#include "LabelPlacement.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace earth_engine {

class Ellipsoid;
class GlyphAtlas;
class IconAtlas;
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

/// 高程模式(P3 贴地方案 A:CPU 逐顶点高程采样钳制)。
enum class FeatureAltitudeMode {
    Absolute,        ///< 顶点高度 = 存储高度 + heightOffset(椭球高)
    ClampToGround    ///< 顶点高度 = 地形采样高 + heightOffset(存储高度忽略)
};

/// FeatureRenderLayer 的图层级样式(矢量 P1:字面量样式子集,
/// data-driven 表达式属 P6)。
struct FeatureRenderStyle {
    std::array<float, 4> fillColor{0.25f, 0.55f, 0.95f, 0.35f};
    std::array<float, 4> lineColor{1.00f, 0.80f, 0.10f, 0.90f};
    float lineWidthPx = 4.0f;
    /// 点符号(P5a):billboard 颜色与基准尺寸(px)。尺寸语义随形状:
    /// 内置形状 = 外接方边长/圆直径;位图图标 = 图标**高度**(宽按源图
    /// 宽高比推,不拉伸)。
    std::array<float, 4> pointColor{1.00f, 1.00f, 1.00f, 0.95f};
    float pointSizePx = 14.0f;
    /// 符号图形(P6c,设计 §11):内置形状名(circle/square/triangle/
    /// diamond/star/pin,见 SymbolShape.h)或经 Engine::addIconImage 注入
    /// 的位图图标名。名字两处都不命中 → 回落 circle(不断链)。
    std::string pointImage = "circle";
    /// 图形相对锚点的对齐(Auto = pin 底部对齐、其余居中,对齐 MapLibre
    /// 的 icon-anchor 默认 center 语义 + 图钉的直觉)。
    SymbolAnchor pointAnchor = SymbolAnchor::Auto;
    /// 文字标注(P5b,先无避让全画):取 properties[labelProperty] 为文本,
    /// 锚点=Point 本体/LineString 中点顶点/Polygon 环心;字体经
    /// Engine::setLabelFontData 注入,未注入则不出标注。
    std::string labelProperty = "name";
    std::array<float, 4> labelColor{1.00f, 1.00f, 1.00f, 1.00f};
    std::array<float, 4> labelHaloColor{0.05f, 0.05f, 0.05f, 0.90f};
    float labelSizePx = 28.0f;    ///< 文字行高(px)
    float labelOffsetPx = 18.0f;  ///< 基线抬离锚点(px,屏幕向上)
    float labelHaloPx = 2.0f;     ///< halo 描边宽(px)
    // ---- 数据驱动样式表达式(P6b,设计 §12;空 = 用上面字面量) ----
    // 语义分割(setStyle 校验,越界降级字面量+警告):
    // 颜色表达式 = 数据驱动(镶嵌期逐要素求值烘进顶点色,禁 zoom——
    //   zoom 依赖色需逐帧重烘,后置);
    // 宽度/尺寸表达式 = zoom 驱动(每帧求值进 uniform,禁 properties——
    //   逐要素宽度需顶点属性,后置)。
    StyleExpression::Ptr fillColorExpr;
    StyleExpression::Ptr lineColorExpr;
    StyleExpression::Ptr pointColorExpr;
    StyleExpression::Ptr lineWidthExpr;
    StyleExpression::Ptr pointSizeExpr;
    /// 符号图形表达式(P6c):**数据驱动**(逐要素求值定形状/图标名,禁
    /// zoom —— zoom 驱动换图需逐帧重镶,与颜色同理后置)。求值结果按
    /// 字符串取名;非字符串值/求值失败 → 回落 pointImage 字面量。
    StyleExpression::Ptr pointImageExpr;
    FeatureAltitudeMode altitudeMode = FeatureAltitudeMode::Absolute;
    /// 高程偏移(m),语义见 FeatureAltitudeMode。Clamp 模式下兼作防
    /// z-fight 抬升(地形网格是 65 格下采样,面与网格间存在格内起伏差)。
    double heightOffset = 0.0;
    /// 贴地细分间距(m):边按此长度细分、polygon 内部按此网格撒 Steiner
    /// 点逐点采高,让几何跟随地形起伏(方案 A 是线/面过渡态,stencil B 终态)。
    double clampDensifyMeters = 100.0;
};

/// 地形高程采样注入(P3)。与 Tileset 解耦:Scene 接线真实地形,host 测试
/// 注入合成函数。两个回调都在渲染线程调用。
struct FeatureTerrainSampling {
    /// 针对区域创建批量采样函数(候选瓦片一次收集);返回空 function 或
    /// 本字段为空 = 无地形(钳制回落椭球面,对齐无数据回退约定)。
    std::function<
        std::function<std::optional<float>(double, double)>(const Rectangle&)>
        makeAreaSampler;
    /// 地形数据代次签名:变化 → 已钳制几何节流重钳(方案 A 的 LOD 切换
    /// 重钳,过渡态策略)。
    std::function<uint64_t()> revision;
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

    /// 设样式。已建桶按新样式全部重镶(渲染线程调用)。
    void setStyle(const FeatureRenderStyle& s);
    const FeatureRenderStyle& style() const { return style_; }

    /// 注入地形采样(P3 贴地)。Scene 接线;不设 = 钳制回落椭球面。
    void setTerrainSampling(FeatureTerrainSampling sampling) {
        terrainSampling_ = std::move(sampling);
    }

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

    // ---- 标签避让 placement(P5c,设计 §8.2) ----

    /// 编辑联动:选中要素标签提权(避让最先入格,几乎总能显示)。
    /// kInvalidFeatureId = 清除。应用层选中态变化时调用。
    void setLabelPriorityFeature(FeatureId id) {
        labelPlacement_.setPriorityFeature(id);
    }

    /// 该要素标签当前渐变后透明度(0 = 避让隐藏/无标签;测试/诊断)。
    float labelOpacityForFeature(FeatureId id) const {
        return labelPlacement_.opacity(id);
    }

    /// 上一帧 placement 计数(测试/诊断)。
    const LabelPlacementStats& labelPlacementStats() const {
        return labelPlacement_.stats();
    }

private:
    /// 单要素标签在桶标签顶点流中的登记(P5c placement 的 collect 源 +
    /// opacity 回写区间)。碰撞盒 px 相对锚点投影位置(y 向上,含 halo)。
    struct LabelEntry {
        FeatureId featureId = kInvalidFeatureId;
        Vec3 anchorEcef;               ///< 绝对 ECEF(double,不减桶原点)
        float boxMinXPx = 0.0f;
        float boxMinYPx = 0.0f;
        float boxMaxXPx = 0.0f;
        float boxMaxYPx = 0.0f;
        size_t vertexFloatStart = 0;   ///< labelVerts 起始 float 下标
        size_t vertexFloatCount = 0;
        float appliedOpacity = 0.0f;   ///< 已写入顶点流的值(判重传)
    };

    /// 单桶常驻 GPU 几何。fill/line/point 任一可空(indexCount=0)。
    struct BucketGpu {
        Vec3 origin = Vec3::zero();        ///< ECEF double 原点
        std::unique_ptr<Buffer> fillVertexBuffer;
        std::unique_ptr<Buffer> fillIndexBuffer;
        int fillIndexCount = 0;
        std::unique_ptr<Buffer> lineVertexBuffer;
        std::unique_ptr<Buffer> lineIndexBuffer;
        int lineIndexCount = 0;
        std::unique_ptr<Buffer> pointVertexBuffer;
        std::unique_ptr<Buffer> pointIndexBuffer;
        int pointIndexCount = 0;
        std::unique_ptr<Buffer> labelVertexBuffer;
        std::unique_ptr<Buffer> labelIndexBuffer;
        int labelIndexCount = 0;
        /// 标签 CPU 侧:顶点流副本(opacity 分量可改写重传)+ 登记表。
        std::vector<float> labelVertsCpu;
        std::vector<LabelEntry> labelEntries;
        /// P6 stencil 分类贴地(方案 B):面 fill 的水密挤出体(pos-only
        /// 12B,相对桶原点)。P6b 按解析 fill 色分组——每组一对
        /// Volume/Color 命令(组内并集计数,不同色互不污染)。非空 →
        /// 该桶 clamp 面走 stencil 双 pass,不再产出方案 A 的 fill 网格。
        struct VolumeGroupGpu {
            std::array<float, 4> color{0, 0, 0, 1};
            std::unique_ptr<Buffer> vertexBuffer;
            std::unique_ptr<Buffer> indexBuffer;
            int indexCount = 0;
        };
        std::vector<VolumeGroupGpu> volumeGroups;
    };

    /// stencil 体积的 CPU 侧按色分组(key = RGBA8 打包)。
    struct VolumeCpuGroup {
        std::array<float, 4> color{0, 0, 0, 1};
        std::vector<float> verts;
        std::vector<uint32_t> indices;
    };
    using VolumeCpuGroups = std::map<uint32_t, VolumeCpuGroup>;

    /// 重镶单桶:镶嵌桶内全部要素 → 减原点转 float → 建 buffer。
    /// 桶空/全退化 → 从 buckets_ 移除。预览摘除中的要素跳过。
    void rebuildBucket(BucketKey key);

    /// 区域采样函数(空 = 无地形或 Absolute 模式)。
    using AreaSampleFn = std::function<std::optional<float>(double, double)>;

    /// 为一组 rings 的包围区域创建采样函数(Clamp 模式且已注入采样时)。
    AreaSampleFn makeClampSampler(
        const std::vector<std::vector<Cartographic>>& rings) const;

    /// 贴地预变换:边按 clampDensifyMeters 细分 + 逐顶点高度 = 采样 +
    /// offset;polygon 另产出内部网格 Steiner 点(CDT 散点,面披盖地形)。
    Feature prepareClampedFeature(const Feature& feature,
                                  const AreaSampleFn& sample,
                                  std::vector<Cartographic>* outSteiner) const;

    /// 镶嵌单要素几何并追加进 CPU 侧数组(rebuildBucket 与预览路径共用)。
    /// sample 非空 → 先做贴地预变换。
    void tessellateFeatureInto(const Feature& feature,
                               const AreaSampleFn& sample,
                               Vec3& origin,
                               bool& hasOrigin,
                               std::vector<float>& fillVerts,
                               std::vector<uint32_t>& fillIndices,
                               std::vector<float>& lineVerts,
                               std::vector<uint32_t>& lineIndices,
                               std::vector<float>& pointVerts,
                               std::vector<uint32_t>& pointIndices,
                               std::vector<float>& labelVerts,
                               std::vector<uint32_t>& labelIndices,
                               std::vector<LabelEntry>& labelEntries,
                               VolumeCpuGroups& volumeGroups) const;

    /// P6 stencil 贴地:polygon footprint 挤成水密体(底/顶两层同拓扑
    /// CDT cap + 环边墙),按解析 fill 色归组。高度范围 = 环顶点+粗内部
    /// 网格采样 min/max ± margin;无采样器回落 ±kVolumeMarginMeters。
    void appendFillVolume(const Feature& feature,
                          const AreaSampleFn& sample,
                          const std::array<float, 4>& fillColor,
                          Vec3& origin,
                          bool& hasOrigin,
                          VolumeCpuGroups& volumeGroups) const;

    /// CPU 数组 → BucketGpu(buffer 创建;全空返回 false)。
    bool uploadBucketGpu(const Vec3& origin,
                         const std::vector<float>& fillVerts,
                         const std::vector<uint32_t>& fillIndices,
                         const std::vector<float>& lineVerts,
                         const std::vector<uint32_t>& lineIndices,
                         const std::vector<float>& pointVerts,
                         const std::vector<uint32_t>& pointIndices,
                         std::vector<float>&& labelVerts,
                         const std::vector<uint32_t>& labelIndices,
                         std::vector<LabelEntry>&& labelEntries,
                         const VolumeCpuGroups& volumeGroups,
                         BucketGpu& out) const;

    /// P5c:每帧跑 placement(collect 全桶 LabelEntry → place/commit),
    /// opacity 有变的桶改写 CPU 副本 opacity 分量并 updateBuffer 重传。
    /// 视口桶裁剪:保守地平线圆——相机星下点为中心,角半径 = 相机地平线角
    /// + 要素最大海拔的地平线延伸。圆外的地表点从相机位置纯几何不可见
    /// (与视锥朝向无关),圆内保守纳入不做视锥细判;oversized 桶恒纳入,
    /// 反经线跨界拆两段查询。返回按 key 排序去重。
    std::vector<BucketKey> visibleBucketKeys(
        const FrameState& frameState) const;
    void updateLabelPlacement(const FrameState& frameState,
                              const std::vector<BucketKey>& visibleKeys);

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

    // ---- 贴地(P3 方案 A) ----
    FeatureTerrainSampling terrainSampling_;
    uint64_t lastClampRevision_ = 0;
    uint64_t lastReclampFrameId_ = 0;

    // ---- 文字标注(P5b) ----
    // buildRenderCommands 每帧缓存(编辑预览/重镶路径无 Renderer 引用);
    // 字体就绪状态翻转 → 全部桶重镶补标注。
    GlyphAtlas* glyphAtlas_ = nullptr;
    bool lastAtlasReady_ = false;

    // ---- 位图图标(P6c) ----
    // 与字体同构:图标可在建桶之后才注入,图集代次变化 → 全桶重镶补 uv。
    IconAtlas* iconAtlas_ = nullptr;
    uint64_t lastIconRevision_ = 0;

    // ---- 标签避让 placement(P5c) ----
    LabelPlacement labelPlacement_;
};

} // namespace earth_engine
