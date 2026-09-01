#pragma once

#include "../data/FeatureStore.h"
#include "../data/FeatureTileMesh.h"
#include "../data/StyleExpression.h"
#include "../renderer/RenderCommand.h"
#include "../renderer/SymbolShape.h"
#include "../core/async/WorkLedger.h"
#include "../core/math/Rectangle.h"
#include "../core/math/Mat4.h"
#include "../debug/PlatformLog.h"
#include "../core/math/Vec3.h"
#include "../tiling/TileKey.h"
#include "LabelPlacement.h"

#include <array>
#include <functional>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace earth_engine {

class Ellipsoid;
class AmapClassicRuntime;
class GlyphAtlas;
class IconAtlas;
class RenderDevice;
class Renderer;
class ProjectedPathSampler;
struct ProjectedPathSample;
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
    /// Fill = 存储坐标系下的地表 anchor（统一 Scene 结果的渲染态位置
    /// 另见 renderedPosition）。
    Cartographic position;

    /// 与 worldPosition 对应的渲染大地坐标（含样式/地形高度）。
    Cartographic renderedPosition;

    /// 命中点在 ECEF 中的渲染位置（含 heightOffset / ClampToGround）。
    /// 统一 Scene picking 用它与地形/其它对象比较射线距离；编辑调用方
    /// 仍应使用 position（存储坐标，不含样式偏移）进行几何编辑。
    Vec3 worldPosition = Vec3::zero();
    /// 从相机到 worldPosition 的射线距离（meter）。
    double distanceMeters = 0.0;

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
    enum class LineCap : uint8_t { Butt = 0, Square = 1, Round = 2 };
    enum class LabelDirection : uint8_t { Center, Right, Left, Top, Bottom };
    /// Selects one complete style-consumption contract for the layer.
    /// AmapClassicOfficial is fail-closed: official paint ranges are required,
    /// CSS pixels use AMap's binary retina backing scale, and renderer-local
    /// dash/default paths are not consulted.
    enum class ProviderContract : uint8_t {
        Generic = 0,
        AmapClassicOfficial = 1,
    };

    enum class OfficialRequirement : uint32_t {
        DrawOrder = 1u << 0,
        ZoomWindow = 1u << 1,
        Rank = 1u << 2,
        FillIdentity = 1u << 3,
        LineIdentity = 1u << 4,
        LabelIdentity = 1u << 5,
        PointIdentity = 1u << 6,
    };

    /// Closed provider-owned profiles. Callers may install complete official
    /// scopes, but cannot assemble an arbitrary half-official requirement set.
    enum class AmapClassicScope : uint8_t {
        Surface,
        Transport,
        RoadLabel,
        Poi,
    };

    /// Capabilities installed by the official AMap style modules. Installers
    /// only add requirements, so composing surface/transport/label contracts
    /// cannot accidentally weaken an already-installed official scope.
private:
    ProviderContract providerContract_ = ProviderContract::Generic;
    uint32_t officialRequirements_ = 0;
    uint8_t officialGeometryMask_ = 0x07;
    void installAmapClassicScope(AmapClassicScope scope);
    friend class FeatureRenderLayer;
    friend class AmapClassicStyleContract;

public:
    bool requiresOfficial(OfficialRequirement requirement) const {
        return providerContract_ == ProviderContract::AmapClassicOfficial &&
               (officialRequirements_ & static_cast<uint32_t>(requirement)) != 0;
    }

    bool usesOfficialProviderContract() const {
        return providerContract_ == ProviderContract::AmapClassicOfficial;
    }

    bool admitsGeometry(GeometryType type) const {
        if (!usesOfficialProviderContract()) return true;
        return (officialGeometryMask_ &
                (1u << static_cast<uint8_t>(type))) != 0;
    }

    struct ProviderLabelLayout {
        enum class IconAnchor : uint8_t {
            NumericTopLeft,
            BottomCenter,
        };

        LabelDirection direction = LabelDirection::Center;
        float offsetXPx = 0.0f;
        float offsetYPx = 0.0f;
        float iconWidthPx = 0.0f;
        float iconHeightPx = 0.0f;
        float iconAnchorXPx = 0.0f;
        float iconAnchorYPx = 0.0f;
        IconAnchor iconAnchor = IconAnchor::NumericTopLeft;
        /// Official q8t background frame. Its final size is derived after
        /// glyph measurement; an empty name means an ordinary fixed icon.
        std::string dynamicBackgroundImage;
    };

    struct ResolvedPointStyle {
        bool enabled = false;
        std::optional<ProviderLabelLayout> labelLayout;
        std::optional<double> minZoom;
        std::optional<double> maxZoom;
        std::string image;
        int officialIconAtlas = 0;
        int officialDynamicBackgroundAtlas = 0;
        float sizePx = 0.0f;
        std::array<float, 4> color{1, 1, 1, 1};
        bool officialCanCovered = false;
    };

    struct LineDashPattern {
        /// MapLibre/Amap line-dasharray values, measured in multiples of the
        /// effective stroke width.  count must be 0, 2, or 4; 0 is solid.
        std::array<float, 4> lengths{0.0f, 0.0f, 0.0f, 0.0f};
        uint8_t count = 0;
        /// Analytic cap applied at each visible dash span. Solid open-line
        /// endpoint caps require tessellated endpoint primitives separately.
        LineCap cap = LineCap::Butt;
    };
    /// 固定样式绘制层级。数值越大越晚绘制；同一图层可用
    /// paintOrderExpr 按要素属性细分(例如 30001 kind61/kind63)。
    int paintOrder = kDefaultVectorPaintOrder;
    StyleExpression::Ptr paintOrderExpr;
    /// Geometry-independent fill identity. Sorting remains paintOrder-driven;
    /// this selects command-time polygon colors without rebuilding geometry.
    StyleExpression::Ptr fillStyleGroupExpr;
    /// Optional label-only ordering/style key. Point geometry keeps
    /// paintOrderExpr batching while text may select a small semantic palette.
    StyleExpression::Ptr labelPaintOrderExpr;
    /// Geometry-independent label style identity. Label draw ordering remains
    /// labelPaintOrder/paintOrder-driven; this key selects size/color/halo.
    StyleExpression::Ptr labelStyleGroupExpr;
    /// Optional O(1) composite-property override for label semantic groups.
    /// The key is `valueA:valueB`; this complements the
    /// expression fallback for large official taxonomies without building a
    /// linear match tree or splitting point geometry.
    std::string labelStyleGroupPropertyA;
    std::string labelStyleGroupPropertyB;
    std::unordered_map<std::string, int> labelStyleGroupByProperty;
    /// Data-driven line command style group. Empty means no semantic group.
    /// This does not affect sorting; it only selects command-time line tables.
    StyleExpression::Ptr lineStyleGroupExpr;
    std::array<float, 4> fillColor{0.25f, 0.55f, 0.95f, 0.35f};
    std::array<float, 4> lineColor{1.00f, 0.80f, 0.10f, 0.90f};
    /// Optional property-keyed line colors.  Values are resolved during
    /// tessellation and baked per feature, avoiding one draw/range per
    /// transit route while retaining an extensible data-driven table.
    std::string lineColorProperty;
    std::unordered_map<std::string, std::array<float, 4>>
        lineColorByProperty;
    float lineWidthPx = 4.0f;
    /// Screen-space round joins. Open endpoints remain butt caps, matching
    /// Amap classic-normal solid roads. Currently used by non-clamped ribbon
    /// geometry; terrain-clamp source keeps its two-vertices-per-point ABI.
    bool lineRoundJoin = false;
    /// Optional generic same-name suppression. Provider-owned road-name
    /// records leave this disabled because the official stream already owns
    /// candidate segmentation/admission.
    float lineLabelRepeatDistancePx = 0.0f;
    float lineLabelLetterSpacingEm = 0.0f;
    float lineLabelPaddingXPx = 0.0f;
    float lineLabelPaddingYPx = 0.0f;
    /// 可选道路外描边：复用同一线 vertex/index buffer，先发更宽的纯色
    /// command，再发原逐要素颜色的中心线。不增加 tessellation 或上传。
    bool lineCasingEnabled = false;
    float lineCasingExtraWidthPx = 3.0f;
    /// >0 时按中心线宽度的比例计算额外总宽，并以 ExtraWidthPx 为上限。
    /// 例如 ratio=0.5：2px 支路只加 1px，6px 主干加 3px。
    float lineCasingWidthRatio = 0.0f;
    std::array<float, 4> lineCasingColor{1.0f, 1.0f, 1.0f, 0.92f};
    /// Geometry-independent stroke widths keyed by semantic styleGroup.
    /// paintOrder remains a sorting key and is never consulted for styling.
    std::unordered_map<int, StyleExpression::Ptr> lineWidthExprByStyleGroup;
    /// Command-time visibility windows for fill semantic groups. Values use
    /// display zoom (AMap style zoom minus one), so high-zoom overlays can be
    /// enabled without rebaking source-z14 geometry or adding draw groups.
    std::unordered_map<int, double> fillMinZoomByStyleGroup;
    std::unordered_map<int, double> fillMaxZoomByStyleGroup;
    /// Zoom-only polygon color keyed by fill styleGroup. Transparent results
    /// suppress the command and therefore preserve provider visibility gaps.
    std::unordered_map<int, StyleExpression::Ptr> fillColorExprByStyleGroup;
    /// Official extrusion face color keyed by semantic building identity.
    /// Missing identities cannot emit extrusion geometry.
    std::unordered_map<int, std::array<float, 4>>
        extrusionRoofColorByStyleGroup;
    std::unordered_map<int, std::array<float, 4>>
        extrusionWallColorByStyleGroup;
    std::unordered_map<int, StyleExpression::Ptr>
        lineCasingWidthExprByStyleGroup;
    /// Zoom-driven command color override keyed by line style group.  Alpha
    /// zero means no override and keeps the per-feature baked vertex color.
    std::unordered_map<int, StyleExpression::Ptr> lineColorExprByStyleGroup;
    /// Official provider lineType resolved at command time, then mapped to a
    /// pixel dash/cap contract. Keeping the integer curve separate avoids
    /// encoding dash arrays into StyleValue or rebuilding line geometry.
    std::unordered_map<int, StyleExpression::Ptr> lineTypeExprByStyleGroup;
    std::function<std::optional<LineDashPattern>(int)> lineTypeResolver;
    /// Zoom-driven label size curves keyed only by semantic styleGroup. These
    /// are evaluated only when the tile label bucket is baked, so the glyph
    /// atlas and collision boxes stay coherent for a given view zoom.
    std::unordered_map<int, StyleExpression::Ptr>
        labelSizeExprByStyleGroup;
    /// Command-time label color curves keyed by semantic styleGroup.
    /// Existing label ranges already split on this key, so this adds neither
    /// vertex attributes, glyph rebakes, nor draw calls.
    std::unordered_map<int, StyleExpression::Ptr>
        labelColorExprByStyleGroup;
    /// Optional style-level label onset in display zoom. Effective visibility
    /// is max(feature minZoom, this onset), matching Amap's two independent
    /// gates without mutating source metadata.
    std::unordered_map<int, int> labelMinZoomByStyleGroup;
    /// Official style-level exclusive display-zoom ceiling. This is combined
    /// with the provider feature window; it is never inferred from drawOrder.
    std::unordered_map<int, int> labelMaxZoomByStyleGroup;
    std::unordered_map<int, std::vector<std::pair<int, int>>>
        labelZoomWindowsByStyleGroup;
    /// Optional command-uniform casing color per semantic styleGroup.
    std::unordered_map<int, std::array<float, 4>>
        lineCasingColorByStyleGroup;
    /// Optional label halo color keyed by semantic styleGroup. This is a uniform
    /// override and does not split glyph geometry or batches.
    std::unordered_map<int, std::array<float, 4>>
        labelHaloColorByStyleGroup;
    /// Zoom-driven label halo, evaluated per label command. This preserves
    /// official style transitions without rebaking glyph geometry.
    std::unordered_map<int, StyleExpression::Ptr>
        labelHaloColorExprByStyleGroup;
    /// Provider-owned label outline radius in CSS pixels. Official labels
    /// require this table and never read the generic layer-wide labelHaloPx.
    std::unordered_map<int, StyleExpression::Ptr>
        labelHaloWidthExprByStyleGroup;
    struct LineLabelLayout {
        float repeatDistancePx = 0.0f;
        float letterSpacingEm = 0.0f;
        float paddingXPx = 0.0f;
        float paddingYPx = 0.0f;
    };
    std::unordered_map<int, LineLabelLayout>
        lineLabelLayoutByStyleGroup;
    /// Optional zoom-only casing color, evaluated per command. This is the
    /// casing counterpart of lineColorExprByStyleGroup.
    std::unordered_map<int, StyleExpression::Ptr>
        lineCasingColorExprByStyleGroup;
    std::unordered_map<int, StyleExpression::Ptr>
        lineCasingTypeExprByStyleGroup;
    /// Exact semantic style-group allow-list for casing. Empty means every
    /// line may use the layer-wide casing; AMap always installs explicit
    /// official groups so provider drawOrder can never enable a casing.
    std::unordered_set<int> lineCasingStyleGroups;
    /// Casing is primarily a near-view separation cue.  Keep it independently
    /// zoom-gated so broad map views can use thin single-stroke roads while the
    /// center line itself remains visible.
    double lineCasingMinZoom = 0.0;
    double lineCasingMaxZoom = 24.0;
    /// 图层可见 zoom 窗口(web 墨卡托惯例 zoom ≈ log2(赤道周长/视高),
    /// 与 widthExpr 口径一致)。默认 [0, 24] 恒可见。粗源 LOD 用它做
    /// **近景让位**:z10 面源在 zoom > maxZoom 时整体不渲染(命令不发、
    /// 不参与收敛),由主源 z12-14 细面承接 —— 否则粗像素块与细面叠加
    /// 会呈现「破破烂烂」的双层边。
    double minZoom = 0.0;
    double maxZoom = 24.0;
    /// dash(P6d 收尾):period = 一节「划+空」总长(m,贴地世界米制,
    /// 随透视近大远小),0 = 实线;onFraction = 划段占比 ∈ (0,1]。
    /// stencil 线与方案 A ribbon 两路径同语义。
    float lineDashPeriodMeters = 0.0f;
    float lineDashOnFraction = 0.6f;
    /// Screen-style dash arrays are deliberately separate from the world-
    /// meter demo dash above. They are keyed only by semantic styleGroup and
    /// by stroke phase so an Amap casing can be dashed while its center stays
    /// solid (or vice versa) without duplicating geometry or draw calls.
    std::unordered_map<int, LineDashPattern> lineDashByStyleGroup;
    std::unordered_map<int, LineDashPattern> lineCasingDashByStyleGroup;
    /// Command-time solid open-end cap keyed by resolved styleGroup. Values
    /// are LineCap numerics and may vary with display zoom. Presence also
    /// opts that styleGroup into endpoint-cap primitives at tessellation time.
    std::unordered_map<int, StyleExpression::Ptr> lineSolidCapExprByStyleGroup;
    /// Independent casing endpoint contract. Missing entries remain Butt.
    std::unordered_map<int, StyleExpression::Ptr>
        lineCasingSolidCapExprByStyleGroup;
    /// Optional display-zoom visibility per line style group.  These gates
    /// are command-time and therefore do not rebuild or duplicate geometry.
    std::unordered_map<int, double> lineMinZoomByStyleGroup;
    std::unordered_map<int, double> lineMaxZoomByStyleGroup;
    /// 海拔着色轨迹(demo,2026-08-23):按顶点椭球高在
    /// [lineColorGradientHeightMinMeters, lineColorGradientHeightMaxMeters]
    /// 内从 lineColorGradientLow 线性渐变到 lineColorGradientHigh,逐顶点
    /// 烘进 a_color(RGBA8)。复用既有 VectorLine48 顶点布局与 shader,
    /// 不新增属性/着色器;lengthSoFar 仍照常携带(dash 语义不变)。
    /// 仅作用于方案 A ribbon 线(LineString/outline);stencil 贴地线
    /// (体积 mesh 按整线分组色)不支持逐顶点色,置位时仍按字面量。
    bool lineColorGradientByHeight = false;
    float lineColorGradientHeightMinMeters = 0.0f;
    float lineColorGradientHeightMaxMeters = 3000.0f;
    std::array<float, 4> lineColorGradientLow{0.10f, 0.55f, 0.25f, 0.95f};
    std::array<float, 4> lineColorGradientHigh{0.90f, 0.15f, 0.15f, 0.95f};
    /// 点符号(P5a):billboard 颜色与基准尺寸(px)。尺寸语义随形状:
    /// 内置形状 = 外接方边长/圆直径;位图图标 = 图标**高度**(宽按源图
    /// 宽高比推,不拉伸)。
    std::array<float, 4> pointColor{1.00f, 1.00f, 1.00f, 0.95f};
    float pointSizePx = 14.0f;
    /// Generic browser-provider styles may express screen values in CSS
    /// pixels. AMap classic never reads this compatibility switch: its
    /// ProviderContract owns the exact binary 1x/2x backing scale.
    bool scaleStylePixelsByDevicePixelRatio = false;
    /// Optional discrete zoom selector for provider visibility records.
    /// 0 disables quantization. AMap classic sets 0.8: retain floor until
    /// fraction .8, then select ceil. This affects feature/style min-max
    /// windows only; the layer-level continuous LOD window remains unchanged.
    double visibilityZoomCeilFraction = 0.0;
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
    /// 逐要素文字字号，镶嵌期按 properties 求值；禁用 camera zoom。
    StyleExpression::Ptr labelSizeExpr;
    /// 逐要素文字基线偏移，镶嵌期按 properties 求值；允许 0，禁用 zoom。
    StyleExpression::Ptr labelOffsetExpr;
    StyleExpression::Ptr lineWidthExpr;
    StyleExpression::Ptr pointSizeExpr;
    /// 符号图形表达式(P6c):**数据驱动**(逐要素求值定形状/图标名,禁
    /// zoom —— zoom 驱动换图需逐帧重镶,与颜色同理后置)。求值结果按
    /// 字符串取名;非字符串值/求值失败 → 回落 pointImage 字面量。
    StyleExpression::Ptr pointImageExpr;
    /// Optional late-bound adapter for provider-owned icon taxonomies. The
    /// worker preserves the two identity properties below; the resolver runs
    /// only when display zoom and the current icon atlas are known. Returning
    /// disabled keeps the feature label-only instead of fabricating a shape.
    std::string pointStylePropertyA;
    std::string pointStylePropertyB;
    /// Under AmapClassicOfficial, appearance, layout, sizing, admission, and
    /// collision come only from pointStyleResolver; generic point fields are
    /// never consulted.
    std::function<ResolvedPointStyle(const std::string&, const std::string&,
                                     const std::string&, double, float)>
        pointStyleResolver;
    /// Worker-time closed-contract admission for provider point identities.
    /// This runs before TileSymbolCpu allocation/budgeting; it must accept the
    /// union of official label, fixed-icon, and dynamic-background records.
    std::function<bool(const std::string&, const std::string&)>
        pointIdentityValidator;
    FeatureAltitudeMode altitudeMode = FeatureAltitudeMode::Absolute;
    /// 高程偏移(m),语义见 FeatureAltitudeMode。Clamp 模式下兼作防
    /// z-fight 抬升(地形网格是 65 格下采样,面与网格间存在格内起伏差)。
    double heightOffset = 0.0;
    /// 贴地细分间距(m):边按此长度细分、polygon 内部按此网格撒 Steiner
    /// 点逐点采高,让几何跟随地形起伏(方案 A 是线/面过渡态,stencil B 终态)。
    double clampDensifyMeters = 100.0;
    /// E 方案(P1):clamp 线改走几何 ribbon,不做 stencil 墙带。顶点在
    /// 镶嵌期按椭球面细分(worker 拿不到地形采样器,不采高),P2 由
    /// VectorLine VS 采位移高度纹理贴地。置位时 stencil 双 pass 分支被
    /// 跳过(路网是 ribbon 单 pass,避免多山瓦片墙带 fill rate 失控);
    /// fill 不受影响。
    bool terrainClampRibbon = false;
    /// V6 建筑挤出:Polygon 带 amap_height 属性时挤出(墙+顶面,lambert
    /// 顶光),不产平 fill(与 stencil 面互斥)。
    bool buildingExtrusion = true;
    /// 面 fill 走 stencil 分类(P6a,像素级贴地,2-pass)还是方案A 平面
    /// fill(单 pass,贴地采样 + heightOffset 抬升,掠视有轻微视差)。
    /// 底图级大面(水/绿地)近景 stencil fill rate 高,关掉换单 pass。
    bool stencilFillEnabled = true;
    /// 面外环描边开关。**默认关**:高德复刻里面(地块/水/绿地)不描边;
    /// 裁剪到瓦片边界后,外环含瓦片角点,若用路网配色描边会画出
    /// 「从瓦片角发散的灰色射线」。开时描边色取 lineColor/lineColorExpr。
    bool fillOutlineEnabled = false;
    /// VectorFill 地球网格边长上限(米)。>0 时 PolygonTessellator 细分
    /// 约束边并撒内部 Steiner,避免斜视大三角被近平面裁成射线。0 = 关。
    double globeFillMaxEdgeMeters = 0.0;  // [A/B] 射线诊断:关 V30 细分
};

/// 地形高程采样注入(P3)。与 Tileset 解耦:Scene 接线真实地形,host 测试
/// 注入合成函数。两个回调都在渲染线程调用。
struct FeatureTerrainSampling {
    /// 针对区域创建批量采样函数(候选瓦片一次收集);返回空 function 或
    /// 本字段为空 = 无地形(钳制回落椭球面,对齐无数据回退约定)。
    std::function<
        std::function<std::optional<float>(double, double)>(const Rectangle&)>
        makeAreaSampler;
    /// 当前可见地形面签名:变化 → 已钳制几何节流重钳。官方 Scene 接线
    /// 必须包含 TileRenderEntry 来源、网格档、morph、fade 与高度数据代次，
    /// 不能只用 registry heightmap generation。
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
    struct PresentationPolicy {
        /// Provider-independent globe policy. Above this camera height,
        /// symbols use a near-plane depth to avoid terrain-depth slicing of
        /// screen-space billboards. Zero disables the push.
        float symbolDepthPushCameraHeightMeters = 200000.0f;
        /// Minimum icon opacity when the anchor is terrain-occluded. Labels
        /// still fade to zero because translucent text is unreadable noise.
        float symbolOccludedMinOpacity = 0.2f;
    };
    enum class AmapClassicProfile : uint8_t { Main, Regions, Poi };
    FeatureRenderLayer(std::string layerId,
                       RenderDevice* renderDevice,
                       const Ellipsoid& ellipsoid);
    /// 指定 store 分桶 cell 尺寸(radian)。桶=重镶单元,高密度小范围
    /// 数据(MVT 底图)需细桶,默认档位见 FeatureStore 注释。
    FeatureRenderLayer(std::string layerId,
                       RenderDevice* renderDevice,
                       const Ellipsoid& ellipsoid,
                       double bucketCellSizeRadians);
    ~FeatureRenderLayer();

    FeatureRenderLayer(const FeatureRenderLayer&) = delete;
    FeatureRenderLayer& operator=(const FeatureRenderLayer&) = delete;

    /// Rebind after a GPU surface/context lifecycle change. Passing nullptr
    /// releases every GPU-derived bucket while the old context is still valid;
    /// binding a new device rebuilds editable-store buckets, while MVT tile
    /// buckets are repopulated by their Scene-owned source.
    void setRenderDevice(RenderDevice* device);

    const std::string& id() const { return layerId_; }
    void setVisible(bool v);
    bool visible() const { return visible_; }

    FeatureStore& store() { return store_; }
    const FeatureStore& store() const { return store_; }

    /// 设样式。已建桶按新样式全部重镶(渲染线程调用)。
    void setStyle(const FeatureRenderStyle& s);
#if defined(EARTH_ENGINE_TESTING)
    /// Test-only fixture injection for malformed/partial contract coverage.
    /// Production builds cannot install an official style outside the sealed
    /// profile factory.
    void setStyleForContractTest(const FeatureRenderStyle& s);
#endif
#if defined(EARTH_ENGINE_TESTING)
    void installAmapClassicProfile(AmapClassicProfile profile);
#endif
    const FeatureRenderStyle& style() const { return style_; }
    bool hasSealedOfficialProfile() const { return officialProfileSealed_; }
    std::optional<AmapClassicProfile> amapClassicProfile() const {
        return amapClassicProfile_;
    }
    void setPresentationPolicy(PresentationPolicy policy) {
        presentationPolicy_ = policy;
    }
    const PresentationPolicy& presentationPolicy() const {
        return presentationPolicy_;
    }

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

    /// V27:标注子系统是否仍在收敛(三态谓词,状态查询而非一次性事件)。
    /// ① 未烘桶(字形按预算逐帧补,依赖帧循环 drain)
    /// ② 换代后待全量 placement(labelsAwaitingPlacement_)
    /// ③ fade 未收敛(current != target)
    /// 三段链每段依赖帧循环推进却曾无一申报 —— 桶 commit 落在最后一次全量
    /// placement 之后 + 停帧,新标注永远停在 opacity=0(V27 根因,竞态故
    /// "不稳定")。帧门控经 syncLabelWorkTicket 领取 Pumped 票据此续帧。
    bool hasPendingLabelWork() const;

    /// Official-only migration gate: ordinary surface polygons are painted by
    /// the terrain raster overlay once enabled. Buildings/extrusions and line
    /// outlines remain on their existing geometry paths.
    void setOfficialSurfaceFillBaked(bool enabled) {
        if (officialSurfaceFillBaked_ == enabled) return;
        officialSurfaceFillBaked_ = enabled;
        if (officialProfileSealed_) {
            std::vector<BucketKey> keys;
            keys.reserve(buckets_.size());
            for (const auto& entry : buckets_) keys.push_back(entry.first);
            for (BucketKey key : keys) rebuildBucket(key);
        }
    }
    bool officialSurfaceFillBaked() const { return officialSurfaceFillBaked_; }

    /// 七态只读聚合 dump(诊断基建):给定标注,一行看齐全部宿主可见状态,
    /// 把"跨会话逐层插探针"压成"dump 一眼看谁在说谎"(V27 一天五洞的
    /// 排查成本教训)。七态 = 驻留(桶)× 烘焙(settled/indexCount)×
    /// placement(target)× fade(current→target)× 回写(appliedOpacity)
    /// × 锚点代次(clampRev/重钳队列)× 遮挡 —— 第七态是 shader 侧
    /// (T2 eeSymbolTerrainVisibility),宿主读不到,不在本 dump。
    /// nameFilter 非空 = 按标注名子串过滤(无名行只在空过滤时输出)。
    /// 纯读、不推进任何状态;渲染线程调用(读桶表)。
    std::string dumpLabelLifecycle(
        const std::string& nameFilter = std::string()) const;

    /// P4:上一次**全量** placement 的耗时与候选数。哨兵只在 >4ms 报,
    /// 拿不到低负载段的点 —— 判"线性还是超线性"需要整条曲线,不是两个点。
    double lastPlacementMs() const { return lastPlacementMs_; }
    size_t lastPlacementCandidates() const {
        return lastPlacementCandidates_;
    }

    /// 镶嵌所需的全部外部状态。**镶嵌不再读任何成员**,故可在 worker 线程
    /// 跑(E1 瓦片桶路径)。
    ///
    /// 线程契约:
    /// - style 是**值拷贝**:setStyle 在渲染线程写 style_,worker 直接读成员
    ///   就是数据竞争。worker 任务持有自己的快照,任务期内不变。
    /// - glyphAtlas/iconAtlas 必须为 nullptr(worker 侧)—— GlyphAtlas::
    ///   ensureGlyph 会现场栅格化字形并传纹理,IconAtlas 同理,都只能在渲染
    ///   线程。将来若有人在 Polygon/LineString 分支用上图集,worker 路径会
    ///   立刻空指针炸掉,而不是静默竞争。现状:图集只在 Point(图标)与
    ///   label 发射处用到,fill/line 全程不碰。
    /// - ellipsoid 是进程级常量(Ellipsoid::WGS84),引用安全。
    struct TessellationContext {
        FeatureRenderStyle style;
        const Ellipsoid& ellipsoid;
        GlyphAtlas* glyphAtlas = nullptr;
        IconAtlas* iconAtlas = nullptr;
        /// 后端能否做 stencil 分类贴地(P6 方案 B)。原先直接问
        /// renderDevice_->supportsStencilClassification(),但那是**静态能力
        /// 位**,不是真要用设备 —— 快照进来,镶嵌器就彻底不持设备指针。
        bool supportsStencilClassification = false;
        double labelViewZoom = 0.0;
        /// 该批要素所在区域的地形高度范围(米,椭球面之上)。**有值时取代
        /// 逐点地形采样**:stencil 分类是像素级判定,挤出体只需要覆盖住地形
        /// 的高度范围,不需要贴合每个顶点的精确地面高度。
        ///
        /// 这是 worker 路径能贴地的关键 —— worker 拿不到地形采样器(那是
        /// 渲染线程状态),但拿得到一对标量。同一思路见 cesium 的
        /// ApproximateTerrainHeights(它也不逐顶点采样)。
        ///
        /// ⚠️ 范围取窄了体会穿不透地形 → 线/面成片消失;取宽了只是多画几个
        /// 片元(体 pass 是深度-only)。**拿不准时往宽取。**
        bool hasTerrainHeightRange = false;
        double terrainMinHeight = 0.0;
        double terrainMaxHeight = 0.0;
        /// Slow-tile attribution only. Disabled by default so generic users
        /// pay no per-feature clock cost; production AMap sinks opt in.
        bool collectDiagnostics = false;
        FeatureTileMesh::TessellationDiagnostics* tileMeshDiagnostics = nullptr;
        /// Official AMap-only migration gate. When the sealed 256 surface
        /// overlay is installed, ordinary provider surface polygons keep
        /// their topology for the mask fetch path but do not build a second
        /// terrain-clamped CDT fill mesh. Buildings/extrusions are unaffected.
        bool bakeOfficialSurfaceFill = false;
    };

#if defined(EARTH_ENGINE_TESTING)
    void setOfficialIconAtlasDemandForTest(std::function<void(int)> demand) {
        setOfficialIconAtlasDemand(std::move(demand));
    }
#endif

private:
    friend class AmapClassicSourceBundle;
    friend class AmapClassicRuntime;
    void setOfficialIconAtlasDemand(std::function<void(int)> demand) {
        officialIconAtlasDemand_ = std::move(demand);
    }
#if !defined(EARTH_ENGINE_TESTING)
    void installAmapClassicProfile(AmapClassicProfile profile);
#endif
    void applyStyleUnchecked(const FeatureRenderStyle& s);
    bool officialProfileSealed_ = false;
    bool officialSurfaceFillBaked_ = false;
    std::function<void(int)> officialIconAtlasDemand_;
    std::optional<AmapClassicProfile> amapClassicProfile_;
    /// 单要素标签在桶标签顶点流中的登记(P5c placement 的 collect 源 +
    /// opacity 回写区间)。碰撞盒 px 相对锚点投影位置(y 向上,含 halo)。
    struct LabelEntry {
        FeatureId featureId = kInvalidFeatureId;
        int rank = 6;
        uint64_t officialInsertionOrder = 0;
        uint32_t officialFragmentOrder = 0;
        int minZoom = 0;  ///< 要素显示窗口 [minZoom, maxZoom)
        int maxZoom = 30;
        Vec3 anchorEcef;               ///< 绝对 ECEF(double,不减桶原点)
        Vec3 tangentEcef;              ///< 线标签屏幕方向参考；等于 anchor=水平
        float boxMinXPx = 0.0f;
        float boxMinYPx = 0.0f;
        float boxMaxXPx = 0.0f;
        float boxMaxYPx = 0.0f;
        bool hasIconBox = false;
        float iconBoxMinXPx = 0.0f;
        float iconBoxMinYPx = 0.0f;
        float iconBoxMaxXPx = 0.0f;
        float iconBoxMaxYPx = 0.0f;
        uint64_t repeatGroup = 0;
        float repeatDistancePx = 0.0f;
        float angleRad = 0.0f;
        float paddingXPx = 0.0f;
        float paddingYPx = 0.0f;
        bool officialCanCovered = false;
        std::vector<LabelCollisionPart> collisionParts;
        size_t vertexFloatStart = 0;   ///< labelVerts 起始 float 下标
        size_t vertexFloatCount = 0;
        size_t labelIndexStart = 0;
        size_t labelIndexCount = 0;
        size_t backgroundIndexStart = 0;
        size_t backgroundIndexCount = 0;
        float appliedOpacity = 0.0f;   ///< 已写入顶点流的值(判重传)
    };

    struct LabelGeometryCpu {
        std::vector<float> verts;
        std::vector<uint32_t> indices;
        std::vector<uint32_t> backgroundIndices;
        std::vector<LabelEntry> entries;
    };

    /// 单桶常驻 GPU 几何。fill/line/point 任一可空(indexCount=0)。
    struct BucketGpu {
        struct PaintRangeGpu {
            int paintOrder = 0;
            int indexOffset = 0;
            int indexCount = 0;
            int minZoom = 0;  ///< 要素显示窗口 [minZoom, maxZoom)
            int maxZoom = 30;
            int styleGroup = 0;
        };
        Vec3 origin = Vec3::zero();        ///< ECEF double 原点
        std::unique_ptr<Buffer> fillVertexBuffer;
        std::unique_ptr<Buffer> fillIndexBuffer;
        int fillIndexCount = 0;
        std::vector<PaintRangeGpu> fillRanges;
        std::vector<float> fillClampSource;
        std::unique_ptr<Buffer> lineVertexBuffer;
        std::unique_ptr<Buffer> lineIndexBuffer;
        int lineIndexCount = 0;
        std::vector<PaintRangeGpu> lineRanges;
        std::unique_ptr<Buffer> pointVertexBuffer;
        std::unique_ptr<Buffer> pointIndexBuffer;
        int pointIndexCount = 0;
        std::vector<PaintRangeGpu> pointRanges;
        std::unique_ptr<Buffer> labelVertexBuffer;
        std::unique_ptr<Buffer> labelIndexBuffer;
        int labelIndexCount = 0;
        std::vector<PaintRangeGpu> labelRanges;
        std::unique_ptr<Buffer> labelBackgroundIndexBuffer;
        int labelBackgroundIndexCount = 0;
        std::vector<PaintRangeGpu> labelBackgroundRanges;
        /// 标签 CPU 侧:顶点流副本(opacity 分量可改写重传)+ 登记表。
        std::vector<float> labelVertsCpu;
        std::vector<LabelEntry> labelEntries;
        /// 瓦片桶专属:标签烘焙源(rel/锚点/合成 id/文字)。commit 只存源,
        /// bakeTileBucketLabels 在字体就绪时烘 —— 字体注入晚于瓦片 commit
        /// 时,store 桶走 rebuildBucket 补标注,瓦片桶没有重镶路径,靠它。
        struct TileLabelSource {
            struct GenericVisualPayload {
                float labelSizePx = 28.0f;
                float labelOffsetPx = 18.0f;
            };
            int paintOrder = 0;
            int styleGroup = 0;
            int rank = 6;
            uint64_t officialInsertionOrder = 0;
            int minZoom = 0;
            int maxZoom = 30;
            /// Official-provider labels resolve size/offset exclusively from
            /// providerLayout + the sealed styleGroup runtime.  Only generic
            /// labels carry caller-authored visual scalars.
            std::optional<GenericVisualPayload> genericVisual;
            uint64_t repeatGroup = 0;
            float repeatDistancePx = 0.0f;
            float angleRad = 0.0f;
            float letterSpacingEm = 0.0f;
            float paddingXPx = 0.0f;
            float paddingYPx = 0.0f;
            std::optional<FeatureRenderStyle::ProviderLabelLayout>
                providerLayout;
            std::array<float, 3> rel{0.0f, 0.0f, 0.0f};
            Vec3 anchorEcef = Vec3::zero();
            std::array<float, 3> tangentRel{0.0f, 0.0f, 0.0f};
            Vec3 tangentEcef = Vec3::zero();
            std::vector<std::array<double, 3>> pathCartographic;
            uint64_t featureId = 0;
            std::string name;
            std::vector<uint32_t> labelSplitIndicesUtf16;
            bool officialCanCovered = false;
        };
        std::vector<TileLabelSource> tileLabelSources;
        /// 本桶尚未完成的唯一 codepoint 集。首次 bake 构建，之后 Ready /
        /// MissingTerminal 项就地移除；后台期间不再重复 decode 全部 name，
        /// 也不从头扫描已经完成的前缀。
        std::vector<uint32_t> labelRequiredGlyphs;
        bool labelRequiredGlyphsReady = false;
        /// 标签派生 VBO/IBO 的连续创建失败数。瞬时 GPU/驱动失败至少要能
        /// 跨帧重试，不能在稳定视野永久丢标；同时设有限上限，避免永久
        /// OOM 把帧循环钉死。
        int labelUploadFailures = 0;
        /// V27:标注烘焙已达稳态(成功 / 确认无可显示字形 / buffer 失败按
        /// 原语义等翻转重试)。区分"在途(预算没补完,下帧继续)"与"不会
        /// 再有产物"——谓词 hasPendingLabelWork 只把前者算作在途,否则
        /// 一个烘不出字的桶让帧循环永不 idle(白烧)。atlas 翻转 / 重钳
        /// 清位重试。
        bool labelBakeSettled = false;
        Mat4 labelBakeViewProjection;
        double labelBakeViewportWidth = 0.0;
        double labelBakeViewportHeight = 0.0;
        bool hasCameraDependentLabelBake = false;
        /// 瓦片桶专属:符号实例源。AMap provider 保留完整官方候选，
        /// 通用图层可按引擎预算物化子集。留着是为了地形代次变化时重钳 —— 锚点高度是 commit
        /// 当刻的地形采样,冷启动时地形还粗,细化后山体升上来会把锚点埋
        /// 掉(硬件深度 + T2 判定都读它),表现为"标记点闪一下就没"。
        /// store 桶靠 rebuildBucket 重钳,瓦片桶没有重镶路径,靠它。
        std::vector<TileSymbolCpu> tileSymbolSources;
        /// source 所属瓦片 zoom（cross-tile id 量化容差输入）与当前已物化
        /// 的整数 view zoom。commit 保留完整 source；build 每个 zoom 档
        /// 只把 active top-N 展开成 point/label GPU 派生数据。
        int sourceTileZoom = 0;
        int symbolViewZoomBucket = -1;
        uint64_t symbolSelectionSignature = 0;
        /// 瓦片线重钳源(每最终 line vertex 9f:pos/prev/next lon-lat +
        /// side/length/colorPacked,与 lineVertexBuffer 同序)。地形代次变化
        /// 时重采样并重传顶点缓冲(索引不变)，完整保留官方 join/cap。
        std::vector<float> lineClampSource;
        /// P6 stencil 分类贴地(方案 B):面 fill 的水密挤出体(pos-only
        /// 12B,相对桶原点)。P6b 按解析 fill 色分组——每组一对
        /// Volume/Color 命令(组内并集计数,不同色互不污染)。非空 →
        /// 该桶 clamp 面走 stencil 双 pass,不再产出方案 A 的 fill 网格。
        struct VolumeGroupGpu {
            int paintOrder = 0;
            std::array<float, 4> color{0, 0, 0, 1};
            std::unique_ptr<Buffer> vertexBuffer;
            std::unique_ptr<Buffer> indexBuffer;
            int indexCount = 0;
        };
        std::vector<VolumeGroupGpu> volumeGroups;
        /// P6d stencil 贴地线:连续横截面墙带(pos 3f + extrude 3f +
        /// lengthSoFar 1f = 28B,
        /// 相对桶原点;宽度 VS 按眼深挤出)。按解析线色分组,与 fill 的
        /// volumeGroups 分开存(同色 fill/line 不得并组)。非空 → 该桶
        /// clamp 线走 stencil 双 pass,不再产出方案 A 的线 ribbon。
        std::vector<VolumeGroupGpu> lineVolumeGroups;
        /// V6 建筑挤出(pos3+normal3+color4=28B,相对桶原点)。
        std::unique_ptr<Buffer> extrudeVertexBuffer;
        std::unique_ptr<Buffer> extrudeIndexBuffer;
        int extrudeIndexCount = 0;
        std::vector<PaintRangeGpu> extrudeRanges;
        std::vector<float> extrudeClampSource;
    };

    // VolumeCpuGroup / VolumeCpuGroups 已下沉到 data/FeatureTileMesh.h ——
    // worker 现在也产出它(贴地瓦片走 stencil),载荷类型必须在下层。

    /// 重镶单桶:镶嵌桶内全部要素 → 减原点转 float → 建 buffer。
    /// 桶空/全退化 → 从 buckets_ 移除。预览摘除中的要素跳过。
    void rebuildBucket(BucketKey key);

    /// 字体图集换代后，旧标签 UV/顶点缓冲全部失效；保留瓦片原始标签源，
    /// 由后续按帧预算重新烘焙。渲染线程调用。
    void invalidateTileBucketLabels(BucketGpu& gpu);

    /// 区域采样函数(空 = 无地形或 Absolute 模式)。
    using AreaSampleFn = std::function<std::optional<float>(double, double)>;

    /// 为一组 rings 的包围区域创建采样函数(Clamp 模式且已注入采样时)。
    AreaSampleFn makeClampSampler(
        const std::vector<std::vector<Cartographic>>& rings) const;

    /// 渲染线程自用的上下文(图集齐全,样式取当前成员)。
    TessellationContext tessellationContext() const {
        TessellationContext ctx{
            style_, ellipsoid_, glyphAtlas_, iconAtlas_,
            renderDevice_ && renderDevice_->supportsStencilClassification(),
            currentLabelViewZoom_};
        ctx.bakeOfficialSurfaceFill = officialSurfaceFillBaked_;
        return ctx;
    }

public:
    // ================= E1:MVT 瓦片桶(worker 全链镶嵌) =================
    //
    // 与可编辑的 FeatureStore 路径**并行的第二条上游**,汇于同一命令层
    // (appendBucketCommands)。区别在于:
    //   store 路径 = 空间分桶 + 脏桶差分重镶(编辑友好,但整视口灌入时
    //               退化成反复全量重镶,P4 实测 debug 下 16s/帧);
    //   瓦片桶     = 瓦片即桶,worker 直出顶点/索引,渲染线程只上传 +
    //               **整瓦原子替换**。无 store、无差分、无激活预算。
    //
    // v1 边界(刻意):只做 fill/line。point/label 留在 store 路径 ——
    // 它们要图集(必须渲染线程),且底图里量少;而 16s 尖刺的主体正是
    // 两万级 fill/line 要素的镶嵌。贴地同样留在 v1 之外(worker 拿不到
    // 地形采样器;底图现为 Absolute)。

    /// worker 产物类型见 data/FeatureTileMesh.h(放在下层避免 data → layers
    /// 反向依赖)。这里保留别名,调用方写 FeatureRenderLayer::TileMeshCpu 与
    /// 写 FeatureTileMesh 等价。
    using TileMeshCpu = FeatureTileMesh;

    /// 取一份镶嵌上下文供 worker 使用:样式已快照、图集置空(线程契约见
    /// TessellationContext)。**必须在渲染线程调用**,产出可交给 worker。
    ///
    /// @param terrainMinHeight/terrainMaxHeight 该批要素所在区域的地形高度
    ///        范围(米)。给了就走 stencil 贴地(体覆盖该范围,零地形采样);
    ///        缺省 = 不贴地,与此前行为一致。
    ///        ⚠️ **宁宽勿窄**:窄了体穿不透地形,该区域的线会整片消失。
    TessellationContext workerTessellationContext() const {
        TessellationContext ctx{style_, ellipsoid_, nullptr, nullptr,
                                stencilClassificationSupported(), 0.0};
        ctx.hasTerrainHeightRange = hasWorkerTerrainRange_;
        ctx.terrainMinHeight = workerTerrainMinHeight_;
        ctx.terrainMaxHeight = workerTerrainMaxHeight_;
        ctx.bakeOfficialSurfaceFill = officialSurfaceFillBaked_;
        return ctx;
    }
    /// **渲染线程**设定 worker 贴地用的区域高度范围(米)。worker 侧的
    /// tessellate 钩子取 ctx 时读它 —— 值是一对标量,读到上一帧的版本无害
    /// (范围本就是保守量),故不加锁。
    /// 传 min > max 表示"未知" → 退回不贴地(与此前行为一致)。
    void setWorkerTerrainHeightRange(double minHeight, double maxHeight) {
        hasWorkerTerrainRange_ = maxHeight >= minHeight;
        workerTerrainMinHeight_ = minHeight;
        workerTerrainMaxHeight_ = maxHeight;
    }
    /// 一块地形瓦片的高度范围快照(渲染线程产,worker 读)。
    struct TerrainHeightRangeCell {
        Rectangle rect;
        double minHeight = 0.0;
        double maxHeight = 0.0;
    };
    using TerrainHeightRangeCells = std::vector<TerrainHeightRangeCell>;

    /// **渲染线程**发布逐地形瓦片的高度范围快照。worker 侧
    /// workerTessellationContextForArea 按自己那块瓦片的矩形取局部范围 ——
    /// 全屏一个 union 会让平原上的路背着山地的相对高差,而体高直接换算成
    /// fill(宽视野实测:体高从 10km 降到 1km,矢量 GPU 135→19.6ms)。
    ///
    /// 快照按 **shared_ptr 整体替换**发布,worker 用 atomic_load 取:与那对
    /// 标量"读到上一帧无害"的理由相同(范围本就是保守量),但 vector 不能像
    /// 标量那样裸读 —— 读到一半被 clear 是撕裂,不是旧值。
    void setWorkerTerrainHeightRangeCells(
        std::shared_ptr<const TerrainHeightRangeCells> cells) {
        std::atomic_store(&workerHeightCells_, std::move(cells));
    }

    /// 取一份镶嵌上下文,高度范围收窄到 area 覆盖到的那些地形瓦片。
    /// 快照缺失/无瓦片与 area 相交 → 退回全局范围(即
    /// workerTessellationContext() 的行为),绝不产出比全局更窄的范围:
    /// 窄了体穿不透地形 = 该片区整片消失。**worker 线程可调**。
    TessellationContext workerTessellationContextForArea(
        const Rectangle& area) const {
        TessellationContext ctx = workerTessellationContext();
        const auto cells = std::atomic_load(&workerHeightCells_);
        if (!cells || cells->empty()) return ctx;
        double minH = std::numeric_limits<double>::max();
        double maxH = std::numeric_limits<double>::lowest();
        for (const TerrainHeightRangeCell& cell : *cells) {
            if (!cell.rect.intersects(area)) continue;
            minH = std::min(minH, cell.minHeight);
            maxH = std::max(maxH, cell.maxHeight);
        }
        if (maxH < minH) return ctx;
        // 机制信号:局部范围 vs 全局范围的收窄倍率。缺了它,「逐瓦片收窄生效
        // 了」与「快照发布了但块块都退回全局」在任何日志里读数相同 —— 而这两种
        // 情形的 GPU 账单差一个数量级。
        //
        // 报**聚合**而不是抽样单块:镶嵌是按瓦片到达零散发生的,抽样打印会恰好
        // 命中一块"本来就窄"的瓦片,读出 1.02x,而真正贵的那些块一次都没露面
        // (第一版就是这么误导人的)。均值 + 最大值才描述得了分布。
        {
            const double narrowing =
                (maxH - minH) > 0.0
                    ? (ctx.terrainMaxHeight - ctx.terrainMinHeight) /
                          (maxH - minH)
                    : 1.0;
            static std::mutex sStatsMutex;
            static int sCount = 0;
            static double sSum = 0.0;
            static double sMax = 0.0;
            std::lock_guard<std::mutex> lock(sStatsMutex);
            ++sCount;
            sSum += narrowing;
            sMax = std::max(sMax, narrowing);
            // 每 16 块一行 **或** 撞见显著收窄就立刻打:定期行描述总体,
            // 离群行保证"真正贵的那几块"不会被均值淹没 —— 只有定期行时,
            // 一批本来就窄的瓦片会把读数压成 1.06x,而那批贵的一次没露面。
            if ((sCount % 16) == 0 || narrowing >= 1.5) {
                platformLog(LogLevel::Info, "VectorClamp",
                            "perTileRange tiles=%d narrowing avg=%.2fx "
                            "max=%.2fx | last local=%.0f/%.0f global=%.0f/%.0f "
                            "cells=%zu",
                            sCount, sSum / sCount, sMax,
                            minH, maxH,
                            ctx.terrainMinHeight, ctx.terrainMaxHeight,
                            cells->size());
            }
        }
        ctx.hasTerrainHeightRange = true;
        ctx.terrainMinHeight = minH;
        ctx.terrainMaxHeight = maxH;
        return ctx;
    }

    TessellationContext workerTessellationContext(double terrainMinHeight,
                                                  double terrainMaxHeight) const {
        TessellationContext ctx = workerTessellationContext();
        ctx.hasTerrainHeightRange = terrainMaxHeight >= terrainMinHeight;
        ctx.terrainMinHeight = terrainMinHeight;
        ctx.terrainMaxHeight = terrainMaxHeight;
        return ctx;
    }
    /// 后端静态能力位(渲染线程读设备,快照给 worker)。
    bool stencilClassificationSupported() const;

    /// 在 **worker 线程**把一批要素镶嵌成瓦片网格。不触任何成员,全部外部
    /// 状态经 ctx 传入。fill/line 之外的产物(point/label/stencil 体)被
    /// 丢弃 —— v1 边界,见上方说明。
    static TileMeshCpu tessellateTileMesh(const TessellationContext& ctx,
                                          const std::vector<Feature>& features);

    /// **worker 线程**:单个点要素 → TileSymbolCpu 实例(锚点投影 + 样式
    /// 表达式求值 + name/rank 属性抽取),追加进 mesh.symbols。quad 定型
    /// 留给 commitTileMesh(图集是渲染线程状态)。static 同 tessellate
    /// FeatureInto:不碰成员由编译器保证。
    static void appendTileSymbol(const TessellationContext& ctx,
                                 const Feature& feature,
                                 int paintOrder,
                                 TileMeshCpu& mesh);

    /// **worker 线程**:命名折线 → label-only TileSymbolCpu。锚点取折线
    /// 弧长中点；不产生 point quad，也不要求 GlyphAtlas。
    static void appendTileLineLabel(const TessellationContext& ctx,
                                    const Feature& feature,
                                    int paintOrder,
                                    TileMeshCpu& mesh);

    /// Amap letterSpacing is expressed in em and applies only between
    /// drawable glyphs. Kept pure so style decoding and layout share a
    /// deterministic, backend-independent contract.
    static float labelLetterSpacingAdvancePx(size_t drawableGlyphCount,
                                             float letterSpacingEm,
                                             float labelSizePx);
    static int effectiveLabelMinZoom(const FeatureRenderStyle& style,
                                     int paintOrder, int featureMinZoom);
    static int effectiveLabelMaxZoom(const FeatureRenderStyle& style,
                                     int paintOrder, int featureMaxZoom);
    static bool labelStyleVisibleAtZoom(const FeatureRenderStyle& style,
                                        int styleGroup, double gateZoom);
    static float resolvedLabelSizePx(const FeatureRenderStyle& style,
                                     int paintOrder, double viewZoom,
                                     float featureSizePx);
    static std::array<float, 4> resolvedLabelColor(
        const FeatureRenderStyle& style, int paintOrder, double viewZoom);
    static std::array<float, 4> resolvedLabelHaloColor(
        const FeatureRenderStyle& style, int paintOrder, double viewZoom);
    static float resolvedLabelHaloWidthPx(const FeatureRenderStyle& style,
                                          int styleGroup,
                                          double viewZoom);

    /// **渲染线程**:单条文字 → glyph quads(32B 布局)+ LabelEntry 登记。
    /// store 镶嵌与瓦片准入定型共用 —— 标签顶点布局/碰撞盒契约只此一份。
    static void appendLabelTextQuads(GlyphAtlas& atlas,
                                     const FeatureRenderStyle& style,
                                     FeatureId featureId,
                                     const Vec3& anchorEcef,
                                     const Vec3& tangentEcef,
                                     const std::array<float, 3>& rel,
                                     const std::array<float, 3>& tangentRel,
                                     const std::string& text,
                                     const std::vector<uint32_t>*
                                         splitIndicesUtf16,
                                     std::vector<float>& labelVerts,
                                     std::vector<uint32_t>& labelIndices,
                                     std::vector<uint32_t>* backgroundIndices,
                                     std::vector<LabelEntry>& labelEntries,
                                     int rank = 6,
                                     int minZoom = 0,
                                     int maxZoom = 30,
                                     float labelSizePx = -1.0f,
                                     float labelOffsetPx = -1.0f,
                                     float labelHaloPx = -1.0f,
                                     const FeatureRenderStyle::ProviderLabelLayout*
                                         providerLayout = nullptr,
                                     float providerPixelRatio = 1.0f,
                                     uint64_t repeatGroup = 0,
                                     float repeatDistancePx = 0.0f,
                                     float angleRad = 0.0f,
                                     float letterSpacingEm = 0.0f,
                                     float paddingXPx = 0.0f,
                                     float paddingYPx = 0.0f,
                                     const std::vector<std::array<double, 3>>*
                                         pathCartographic = nullptr,
                                     const Ellipsoid* pathEllipsoid = nullptr,
                                     const Vec3* pathOrigin = nullptr,
                                     double pathMetersPerPixel = 0.0,
                                     const IconAtlas* iconAtlas = nullptr,
                                     const ProjectedPathSampler*
                                         projectedPath = nullptr,
                                     const std::vector<ProjectedPathSample>*
                                         officialGlyphSamples = nullptr);

    /// **渲染线程**:上传并整瓦原子替换。mesh 为空 → EmptyTerminal；
    /// 上传失败 → RetryableFailure，调用方保留 CPU mesh 后续重试。
    TileMeshCommitResult commitTileMesh(const TileKey& key,
                                        TileMeshCpu& mesh);
#if defined(EARTH_ENGINE_TESTING)
    void clampTileMeshForTest(TileMeshCpu& mesh) {
        clampTileFillHeights(mesh);
        clampTileExtrusionHeights(mesh);
        clampTileLineHeights(mesh);
    }
    struct LabelCollisionBoundsForTest {
        std::array<float, 4> text{};
        bool hasSecondary = false;
        std::array<float, 4> secondary{};
    };
    std::optional<LabelCollisionBoundsForTest>
    firstTileLabelCollisionBoundsForTest() const;
    std::optional<uint64_t> officialTileLabelInsertionOrderForTest(
        const TileKey& key, const std::string& name) const;
    struct TerrainReclampSnapshotForTest {
        uint64_t appliedRevision = 0;
        size_t pendingBuckets = 0;
        const Buffer* fillVertexBuffer = nullptr;
        const Buffer* lineVertexBuffer = nullptr;
        const Buffer* pointVertexBuffer = nullptr;
        const Buffer* labelVertexBuffer = nullptr;
        const Buffer* extrusionVertexBuffer = nullptr;
        std::optional<Vec3> origin;
        std::optional<double> firstLabelAnchorHeightMeters;
    };
    TerrainReclampSnapshotForTest terrainReclampSnapshotForTest() const;
#endif
    TileMeshCommitResult commitTileMesh(const TileKey& key,
                                        TileMeshCpu&& mesh) {
        return commitTileMesh(key, mesh);
    }

    /// **渲染线程**:移除一块瓦片的 GPU 资源。
    void dropTileMesh(const TileKey& key);

    enum class TileLabelBakeResult {
        Settled,
        Deferred,
        AtlasSaturated,
        RetryableFailure
    };

    /// **渲染线程**:瓦片桶标签烘焙(符号刀B)。桶有标签源且字体就绪且
    /// 尚未烘过 → 生成 glyph quads + LabelEntry + GPU buffer。只在命令
    /// 构建阶段按 Renderer 级共享预算推进；commit 只登记源，避免在帧前
    /// 提交路径绕过预算。字体就绪翻转只清 settled，随后同样由预算 drain。
    /// AtlasSaturated 表示全局字形并发/本帧启动预算已满，调用方应立即停止
    /// 整层桶扫描；下一帧由 hasPendingLabelWork 继续供帧。
    TileLabelBakeResult bakeTileBucketLabels(BucketGpu& gpu,
                                              double viewZoom,
                                              float stylePixelRatio,
                                              const Mat4& viewProjection,
                                              double viewportWidth,
                                              double viewportHeight,
                                              bool forceCameraRebake = false);

    /// P6:Renderer 级共享的新字形栅格化预算。预算所有权在 GlyphAtlas，
    /// 因为同一 Renderer 下可能有多个 FeatureRenderLayer；若放在 layer
    /// 成员，每层重置一次会退化为 N×4ms。单字形不可抢占，因此每帧至少
    /// 放行一个，之后达到时间上限便延迟剩余桶。
    double lastPlacementMs_ = 0.0;
    size_t lastPlacementCandidates_ = 0;
    static constexpr double kGlyphRasterBudgetMs = 4.0;

    /// P6:地形代次重钳的每帧桶预算。重钳一次要重建全部瓦片桶(~60 个)
    /// 的点 buffer + 重烘标签,实测单帧 27ms。代次变化由 2 秒合并窗节流,
    /// 摊几帧完成完全不可见。
    static constexpr int kReclampBucketsPerFrame = 4;
    std::vector<TileKey> pendingReclamp_;

    /// **渲染线程**:符号实例表 → 点 quad + 标签烘焙源(采地面高 + 图集
    /// 解析)。commit 与重钳共用一份 —— 两处各写一遍必然错位。
    void buildTileSymbolGpu(const std::vector<TileSymbolCpu>& symbols,
                            const Vec3& origin, int tileZ,
                            double viewZoom, float officialScale,
                            std::vector<float>& pointVerts,
                            std::vector<uint32_t>& pointIndices,
                            std::vector<PaintRange>& pointRanges,
                            std::vector<BucketGpu::TileLabelSource>& labelSrc);
    bool rebuildTileBucketSymbolsForZoom(BucketGpu& gpu,
                                         int viewZoomBucket,
                                         bool force = false);

    /// **渲染线程**:地形代次变化后按新地形重采锚点高度并重建点/标签
    /// GPU 资源(store 桶的 rebuildBucket 对应物)。无符号源则空转。
    void reclampTileBucketSymbols(BucketGpu& gpu);

    /// E 方案 P2:瓦片线 commit 时按 (lon,lat) 同源采样钳高(worker 只
    /// 给了椭球面高度 + lineClampSource)。
    void clampTileLineHeights(TileMeshCpu& mesh);
    /// E 方案 P2:地形代次变化重钳线桶(镜像 reclampTileBucketSymbols;
    /// 只重建顶点缓冲,索引不变)。
    void reclampTileBucketLines(BucketGpu& gpu);
    void clampTileFillHeights(TileMeshCpu& mesh);
    void reclampTileBucketFills(BucketGpu& gpu);
    void clampTileExtrusionHeights(TileMeshCpu& mesh);
    void reclampTileBucketExtrusions(BucketGpu& gpu);
    /// 共享钳高数学:由 clampSource(每顶点 lon/lat/colorPacked)重建完整
    /// ribbon 顶点流(pos/prev/next/lengthSoFar 重算,side/color 原样)。
    /// 返回 false = 源不足/退化,调用方保留旧缓冲。
    bool reclampLineVertsFromSource(const std::vector<float>& clampSource,
                                    std::vector<float>& outVerts,
                                    const Vec3& origin,
                                    const AreaSampleFn& sample) const;

    /// **渲染线程**:跨瓦稳定符号 ID(符号刀C,对拍 maplibre
    /// CrossTileSymbolIndex 语义)。同名符号锚点在「两代中较粗 zoom 的
    /// MVT 量化格」容差内 → 继承既有 id;miss 分配新 id。placement 的
    /// fade/避让/提权账本按 id 记 —— 瓦片换代(z13→z14)时 id 连续,
    /// 标签不闪不重淡入。索引只增不淘汰:城市级 POI 总量 ~1.4k,全量
    /// 驻留字节级;超容量哨兵打日志再谈 LRU(同 GlyphAtlas 的取向)。
    /// [V29 刀2] claimed = 本次匹配 pass(一次瓦 commit)内已认领的 id 集,
    /// 1:1 贪心:一个既有 entry 每 pass 只许被一个符号继承(maplibre
    /// zoomCrossTileIDs 同款,#5993)。不认领会双认领同一旧 entry(同名多
    /// 实例 fade 互踩)+ hit 路的锚点参考升级被来回拉扯污染后续匹配 ——
    /// 刀1 扩窗后必现,窄窗下亦属正确性。新建的 id 同样入 claimed(防同
    /// pass 后续符号匹配到刚建的 entry 误并)。nullptr = 单条查询(测试/
    /// 诊断),无认领语义。
    uint64_t crossTileIdFor(const std::string& name, double lonRad,
                            double latRad, int tileZ,
                            std::unordered_set<uint64_t>* claimed = nullptr);

    /// 当前驻留的瓦片桶数(诊断)。
    size_t tileMeshCount() const { return tileBuckets_.size(); }

private:

    /// 贴地预变换:边按 densifyMeters 细分 + 逐顶点高度 = 采样 + offset;
    /// polygon 另产出内部网格 Steiner 点(CDT 散点,面披盖地形)。
    /// densifyMeters 由调用方定:方案 A 传 style_.clampDensifyMeters(细分
    /// 兼防露头);stencil 线路径放宽(细分只服务线形曲率 + 高度采样)。
    static Feature prepareClampedFeature(const TessellationContext& ctx,
                                  const Feature& feature,
                                  const AreaSampleFn& sample,
                                  std::vector<Cartographic>* outSteiner,
                                  double densifyMeters);

    /// 镶嵌单要素几何并追加进 CPU 侧数组(rebuildBucket 与预览路径共用)。
    /// sample 非空 → 先做贴地预变换。
    /// static:它已不读任何成员(全部外部状态经 ctx 传入),用 static 让
    /// 「不碰成员」由编译器保证,而不是靠注释自觉——这是 worker 侧安全的
    /// 根据。
    static void tessellateFeatureInto(const TessellationContext& ctx,
                               const Feature& feature,
                               int paintOrder,
                               int fillStyleGroup,
                               int lineStyleGroup,
                               int labelStyleGroup,
                               const AreaSampleFn& sample,
                               Vec3& origin,
                               bool& hasOrigin,
                               PaintGeometryCpu& fillRange,
                               PaintGeometryCpu& lineRange,
                               PaintGeometryCpu& pointRange,
                               LabelGeometryCpu& labelRange,
                               VolumeCpuGroups& volumeGroups,
                               VolumeCpuGroups& lineVolumeGroups,
                               PaintGeometryCpu& extrudeRange);

    static int resolvePaintOrder(const FeatureRenderStyle& style,
                                 const Feature& feature);

    static void flattenPaintRanges(const std::map<int, PaintGeometryCpu>& ranges,
                                   size_t floatsPerVertex,
                                   std::vector<float>& verts,
                                   std::vector<uint32_t>& indices,
                                   std::vector<PaintRange>* outRanges =
                                       nullptr,
                                   std::vector<float>* clampSource = nullptr);
    static void flattenExtrusionRanges(
        const std::map<std::tuple<int, int, int, int>, PaintGeometryCpu>& ranges,
        std::vector<float>& verts, std::vector<uint32_t>& indices,
        std::vector<PaintRange>* outRanges,
        std::vector<float>* clampSource = nullptr);
    static void flattenLinePaintRanges(
        const std::map<std::pair<int, int>, PaintGeometryCpu>& ranges,
        size_t floatsPerVertex, std::vector<float>& verts,
        std::vector<uint32_t>& indices, std::vector<PaintRange>* outRanges,
        std::vector<float>* clampSource = nullptr);
    static void flattenStylePaintRanges(
        const std::map<std::pair<int, int>, PaintGeometryCpu>& ranges,
        size_t floatsPerVertex, std::vector<float>& verts,
        std::vector<uint32_t>& indices,
        std::vector<PaintRange>* outRanges = nullptr);

    static void flattenLabelRanges(
        const std::map<std::pair<int, int>, LabelGeometryCpu>& ranges,
        std::vector<float>& verts,
        std::vector<uint32_t>& indices,
        std::vector<LabelEntry>& entries,
        std::vector<PaintRange>* outRanges,
        std::vector<uint32_t>* backgroundIndices = nullptr,
        std::vector<PaintRange>* backgroundRanges = nullptr);

    /// V6 建筑挤出:footprint(贴地钳高后)+ amap_height → 墙带 + CDT 顶面。
    static void appendExtrusionVolume(
        const TessellationContext& ctx,
        const Feature& feature,
        const std::array<float, 4>& roofColor,
        const std::array<float, 4>& wallColor,
        Vec3& origin,
        bool& hasOrigin,
        std::vector<float>& extrudeVerts,
        std::vector<uint32_t>& extrudeIndices,
        std::vector<float>* clampSource = nullptr);

    /// P6 stencil 贴地:polygon footprint 挤成水密体(底/顶两层同拓扑
    /// CDT cap + 环边墙),按解析 fill 色归组。高度范围 = 环顶点+粗内部
    /// 网格采样 min/max ± margin;无采样器回落 ±kVolumeMarginMeters。
    static void appendFillVolume(const TessellationContext& ctx,
                          const Feature& feature,
                          int paintOrder,
                          const AreaSampleFn& sample,
                          const std::array<float, 4>& fillColor,
                          Vec3& origin,
                          bool& hasOrigin,
                          VolumeCpuGroups& volumeGroups);

    /// P6d stencil 贴地线:细分折线挤成连续横截面墙带(每细分点一个
    /// 4 顶点横截面:高度 = 点采样高 ± margin,CPU 侧零宽;VS 沿烘入的
    /// miter 挤出向量按眼深换算世界半宽)。横截面共享 → 水密免段间缝。
    /// closed = 闭合环(polygon outline,首尾 wrap 无端 cap)。
    static void appendLineVolume(const TessellationContext& ctx,
                          const std::vector<Cartographic>& points,
                          int paintOrder,
                          bool closed,
                          const std::array<float, 4>& lineColor,
                          Vec3& origin,
                          bool& hasOrigin,
                          VolumeCpuGroups& lineVolumeGroups);

    /// CPU 数组 → BucketGpu(buffer 创建;全空返回 false)。
    bool uploadBucketGpu(const Vec3& origin,
                         const std::vector<float>& fillVerts,
                         const std::vector<uint32_t>& fillIndices,
                         const std::vector<PaintRange>& fillRanges,
                         const std::vector<float>& lineVerts,
                         const std::vector<uint32_t>& lineIndices,
                         const std::vector<PaintRange>& lineRanges,
                         const std::vector<float>& pointVerts,
                         const std::vector<uint32_t>& pointIndices,
                         const std::vector<PaintRange>& pointRanges,
                         std::vector<float>&& labelVerts,
                         const std::vector<uint32_t>& labelIndices,
                         const std::vector<PaintRange>& labelRanges,
                         std::vector<LabelEntry>&& labelEntries,
                         const VolumeCpuGroups& volumeGroups,
                         const VolumeCpuGroups& lineVolumeGroups,
                         const std::vector<float>& extrudeVerts,
                         const std::vector<uint32_t>& extrudeIndices,
                         const std::vector<PaintRange>& extrudeRanges,
                         BucketGpu& out) const;

    /// P5c:每帧跑 placement(collect 全桶 LabelEntry → place/commit),
    /// opacity 有变的桶改写 CPU 副本 opacity 分量并 updateBuffer 重传。
    /// 视口桶裁剪:保守地平线圆——相机星下点为中心,角半径 = 相机地平线角
    /// + 要素最大海拔的地平线延伸。圆外的地表点从相机位置纯几何不可见
    /// (与视锥朝向无关),圆内保守纳入不做视锥细判;oversized 桶恒纳入,
    /// 反经线跨界拆两段查询。返回按 key 排序去重。
    std::vector<BucketKey> visibleBucketKeys(
        const FrameState& frameState) const;
    /// fade 后 opacity 回写顶点流(可见 store 桶 + 全部瓦片桶 + 预览)。
    /// 全量 placement 帧与节流间隙的 advanceFades 帧共用。
    void applyLabelOpacity(const std::vector<BucketKey>& visibleKeys);

    void updateLabelPlacement(const FrameState& frameState,
                              const std::vector<BucketKey>& visibleKeys);

    /// V27:按 hasPendingLabelWork 谓词 acquire/release 标注收敛 Pumped 票。
    /// 每帧在 buildRenderCommands **最前**调(该函数多处早退,放后面会被
    /// 跳过 —— syncWorkTicket 同款注意事项);不可见层判不忙(不出命令的层
    /// 不许扣住帧循环)。
    void syncLabelWorkTicket();

    /// T2:给符号命令(点/标注)挂地形深度纹理 + 遮挡参数。纹理恒占
    /// textures[1],通路不可用时挂 nullptr 并把 enabled 置 0。
    void appendTerrainOcclusion(const Renderer& renderer,
                                RenderCommand& cmd) const;

    struct CommandFrameParams {
        Mat4 viewProjection;
        Mat4 view;
        double viewportWidth = 0.0;
        double viewportHeight = 0.0;
        double cameraHeight = 0.0;
        double zoomLevel = 0.0;
        float lineWidthPx = 0.0f;
        float pointSizePx = 0.0f;
        float stylePixelRatio = 1.0f;
        float symbolDepthPush = 0.0f;
        float halfWidthPerEyeZ = 0.0f;
    };

    /// 生成一对 fill/line 命令追加进 commands(常驻桶与预览路径共用)。
    void appendBucketCommands(const BucketGpu& gpu,
                              const FrameState& frameState,
                              const CommandFrameParams& frameParams,
                              Renderer& renderer,
                              RenderCommandList& commands) const;

    std::string layerId_;
    bool visible_ = true;
    PresentationPolicy presentationPolicy_;
    RenderDevice* renderDevice_ = nullptr;
    // worker 贴地用的区域高度范围(渲染线程写,worker 读;见
    // setWorkerTerrainHeightRange 的无锁理由)。
    bool hasWorkerTerrainRange_ = false;
    double workerTerrainMinHeight_ = 0.0;
    double workerTerrainMaxHeight_ = 0.0;
    /// 逐地形瓦片高度范围快照。整体替换发布 + atomic_load 读(见
    /// setWorkerTerrainHeightRangeCells)。
    std::shared_ptr<const TerrainHeightRangeCells> workerHeightCells_;
    const Ellipsoid& ellipsoid_;
    FeatureRenderStyle style_;
    FeatureStore store_;
    std::unordered_map<BucketKey, BucketGpu> buckets_;
    /// E1:MVT 瓦片桶(瓦片即桶)。与 buckets_ 平行,同一命令层消费。
    std::unordered_map<TileKey, BucketGpu> tileBuckets_;
    /// Render-thread equivalent of the official worker Util.stamp stream.
    /// Assigned once when a sealed-provider tile is admitted, then carried
    /// through terrain/zoom/glyph rebuilds. Zero remains the generic sentinel.
    uint64_t nextOfficialInsertionOrder_ = 1;

    // ---- 编辑预览态 ----
    FeatureId previewFeatureId_ = kInvalidFeatureId;
    GeometryType previewType_ = GeometryType::Point;
    std::vector<std::vector<Cartographic>> previewRings_;
    bool previewDirty_ = false;
    BucketGpu previewGpu_;
    bool previewGpuValid_ = false;

    // ---- 贴地(P3 方案 A) ----
    FeatureTerrainSampling terrainSampling_;
    /// 已触发的重钳目标 revision。上一轮 drain 排空后若 revision 仍落后,
    /// 立即再起新一轮;谓词据此在「落后且队列空」时继续供帧。无时间冷却。
    uint64_t lastClampRevision_ = 0;

    // ---- 文字标注(P5b) ----
    // buildRenderCommands 每帧缓存(编辑预览/重镶路径无 Renderer 引用);
    // 字体就绪状态或代次变化 → 全部桶重镶/重烘补标注。
    GlyphAtlas* glyphAtlas_ = nullptr;
    bool lastAtlasReady_ = false;
    uint64_t lastGlyphRevision_ = 0;

    // ---- 位图图标(P6c) ----
    // 与字体同构:图标可在建桶之后才注入,图集代次变化 → 全桶重镶补 uv。
    IconAtlas* iconAtlas_ = nullptr;
    uint64_t lastIconRevision_ = 0;
    float lastStylePixelRatio_ = 1.0f;

    // ---- 跨瓦稳定符号 ID(符号刀C) ----
    /// name 哈希 → 同名符号锚点表。语义见 crossTileIdFor。
    struct CrossTileEntry {
        double lonRad = 0.0;
        double latRad = 0.0;
        int zoom = 0;        ///< 锚点来源瓦片 zoom(越大坐标越准)
        uint64_t id = 0;
    };
    std::unordered_map<uint64_t, std::vector<CrossTileEntry>> crossTileIndex_;
    uint64_t nextCrossTileId_ = 1;
    size_t crossTileEntryCount_ = 0;

    // ---- placement 节流(符号刀D) ----
    /// ≤0 时下一帧跑全量 placement;初值 0 = 首帧即跑(标签不等节流窗)。
    double placementCooldownSeconds_ = 0.0;
    FeatureId lastPlacementPriority_ = kInvalidFeatureId;
    /// POI min/max 都是整数 zoom 门槛；跨整数档立即重跑 placement，
    /// 避免一次性缩放结束后因 300ms 节流而停在旧可见集。
    int lastPlacementZoomBucket_ = -1;
    /// 瓦片标签派生几何只覆盖当前整数 zoom 窗口；跨档时失效并按保留的
    /// tileLabelSources 重烘，避免为当前不可见的数千 POI 预烘字形/quad。
    int lastLabelBakeZoomBucket_ = -1;
    double currentLabelViewZoom_ = 0.0;
    bool symbolBucketsAwaitingRebuild_ = false;
    /// V27:桶换代(bake 出新标注/重镶)→ 下一帧全量 placement 绕过 300ms
    /// 节流(与 priorityChanged 即时重跑同款),runFull 后清位。不即时跑的
    /// 话,新 entries 的 target 永远没人置,停帧窗口内 = 标注隐形。
    bool labelsAwaitingPlacement_ = false;
    /// V27:标注收敛 Pumped 票(placement/fade 只在渲染帧里推进,停帧 =
    /// 永不收敛,Pumped 语义严合)。syncLabelWorkTicket 按 hasPendingLabelWork
    /// 谓词 acquire/release,照抄 TerrainPageStore::syncWorkTicket 模式。
    WorkLedger::Ticket labelWorkTicket_;
    /// 当前相机是否落在本层整体 zoom 范围内。hasPendingLabelWork 是 Scene
    /// 与 WorkLedger 的共同真值，不能让一个已经整体门控掉的层继续持有
    /// labelConverge；由每帧 build 在任何早退前更新。
    bool labelWorkActiveForCurrentView_ = false;

    // ---- 标签避让 placement(P5c) ----
    LabelPlacement labelPlacement_;
};

} // namespace earth_engine
