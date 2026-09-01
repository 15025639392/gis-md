#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "../core/math/Vec3.h"

namespace earth_engine {

/// 渲染线程接纳一块 CPU 瓦片网格的显式结果。
/// Source 只有拿到成功或“确定为空”才能推进 active 状态；GPU 资源创建失败
/// 必须保留 CPU 网格并在后续帧重试，不能把失败误记成已经上屏。
enum class TileMeshCommitResult {
    Committed,
    EmptyTerminal,
    RetryableFailure,
};

/// stencil 分类体的 CPU 侧按 paint ordinal + 色分组。
///
/// 放在 data/ 与 FeatureTileMesh 同层,是因为 worker 现在也产出它(贴地
/// 瓦片走 stencil 双 pass):生产方在 data/,消费方 FeatureRenderLayer 在
/// layers/,载荷类型必须在下层,否则 data → layers 反向依赖。
///
/// **按 ordinal + 色而不是按要素分组**:同层级同色要素的顶点/索引取
/// 并集进同一组,一组一对 draw(体 pass + 色 pass)。所以 draw call 随
/// **层级/颜色种类数**增长,不随要素数增长；贴地几何也不会跨层级合并。
struct VolumeCpuGroup {
    int paintOrder = 0;
    std::array<float, 4> color{0, 0, 0, 1};
    std::vector<float> verts;
    std::vector<uint32_t> indices;
};
using VolumeCpuGroups =
    std::map<std::pair<int, uint32_t>, VolumeCpuGroup>;

/// 一类矢量几何在固定 paint ordinal/style identity 下的 CPU 分段。
/// 顶点/索引在 commit 时按 ordinal flatten 到单个 VBO/IBO；这里保留
/// 分段是为了不依赖 PBF feature 顺序，并让跨瓦片命令可以复用同一排序键。
struct PaintGeometryCpu {
    std::vector<float> verts;
    std::vector<uint32_t> indices;
    /// 几何同源钳高数据。fill 每顶点为 lon/lat/color 3f；line 使用
    /// FeatureRenderLayer 定义的完整 9f line-vertex 布局。
    std::vector<float> clampSource;
};
struct PaintRange {
    int paintOrder = 0;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    /// 仅点符号范围消费；其它几何保持默认全档可见。
    int minZoom = 0;
    int maxZoom = 30;
    /// Command-style discriminator orthogonal to draw order. Zero means no
    /// provider visual identity; it never aliases paintOrder.
    int styleGroup = 0;
};

/// 瓦片点符号实例(worker 产出的中间表)。
///
/// 点符号**不在 worker 侧定型 quad**:图集查找(位图帧 uv)是渲染线程状态。
/// worker 把纯计算部分全部做完(锚点投影、表达式求值出颜色/图形名、属性
/// 抽取),渲染线程准入时只做「图集解析 + 展开 4 顶点」——一瓦一次,非逐帧。
struct TileSymbolCpu {
    struct GenericVisualPayload {
        float colorPacked = 0.0f;
        float labelSizePx = 28.0f;
        float labelOffsetPx = 18.0f;
        bool iconEnabled = true;
        std::string icon;
    };
    /// 要素级绘制层级；渲染线程定型 point/label quad 时继续按该值分段。
    int paintOrder = 0;
    int labelPaintOrder = 0;  ///< text-only draw order; zero is a valid value
    int labelStyleGroup = 0;  ///< visual identity, independent of draw order
    /// 锚点大地坐标(radian/meter,**原始几何高**,未含样式 offset)。
    /// 存经纬度而非 ECEF:贴地模式的地形采样是渲染线程状态,准入定型时
    /// 才能把锚点落到地面 —— worker 侧给 ECEF 就把高度焊死在椭球面了
    /// (山地会整批埋进地形被遮挡判定吃掉)。
    double lonRad = 0.0;
    double latRad = 0.0;
    double heightM = 0.0;
    /// Generic provider-only visual payload. Official AMap symbols never
    /// allocate this payload: they carry identity and placement data only,
    /// and the render thread resolves every visual field from the sealed
    /// provider contract.
    std::optional<GenericVisualPayload> genericVisual;
    int rank = 6;              ///< 数据侧重要度(小=重要),准入截断依据
    /// Sealed AMap labels receive a render-thread admission sequence that is
    /// equivalent to the worker's monotonically increasing Util.stamp id.
    /// Zero is reserved for generic providers.  The value survives zoom and
    /// terrain rebuilds so equal-rank placement never depends on hash-map
    /// traversal order or camera distance.
    uint64_t officialInsertionOrder = 0;
    /// 数据侧显示窗口 [minZoom,maxZoom)。Amap POI 的 min/max 是要素级
    /// zoom 语义，不能
    /// 用 source tile z 代替；普通本地点默认全档可见。
    int minZoom = 0;
    int maxZoom = 30;
    /// 同名线标签的重复组与最小屏幕间距。点标签保持 0，不改变既有语义；
    /// 线标签可按样式/数据源扩展为不同 repeatDistance，而非硬编码在碰撞器。
    uint64_t labelRepeatGroup = 0;
    float labelRepeatDistancePx = 0.0f;
    /// 折线标签中点的局部地理切线角(rad, east/north 平面)。仅供后续
    /// VectorLabel 方向 ABI 消费；普通点标签保持 0。
    float labelAngleRad = 0.0f;
    std::vector<std::array<double, 3>> labelPathCartographic;
    float labelLetterSpacingEm = 0.0f;
    float labelPaddingXPx = 0.0f;
    float labelPaddingYPx = 0.0f;
    std::string pointStyleKeyA;  ///< late-bound provider icon identity A
    std::string pointStyleKeyB;  ///< late-bound provider icon identity B
    std::string name;          ///< 标签文字(文字刀期用,先携带免二次解码)
    std::vector<uint32_t> labelSplitIndicesUtf16;
};

/// 一块瓦片镶嵌后的 CPU 顶点/索引(E1 worker 全链镶嵌的产物)。
///
/// 放在 data/ 而非 layers/ 是为了分层:生产方 MvtVectorSource 在 data/,
/// 消费方 FeatureRenderLayer 在 layers/,把载荷类型放在下层两边都能用,
/// 避免 data → layers 的反向依赖。本结构只依赖 Vec3 与标准容器。
///
/// 顶点是**相对 origin 的 float**(origin 为 double ECEF),与 store 路径的
/// 桶原点 RTE 约定一致 —— 每帧以 double 算 mvp = viewProj·translate(origin)
/// 后降 float,消除 ECEF 直存 float 的米级抖动。
///
/// point/label 需要图集(必须渲染线程),留在 store 路径。
struct FeatureTileMesh {
    struct TessellationDiagnostics {
        enum class RejectionReason : uint8_t {
            GeometryType,
            DrawOrder,
            ZoomWindow,
            FillIdentity,
            LineIdentity,
            PointIdentity,
            Rank,
            DegenerateGeometry,
            LabelLayout,
            Count,
        };
        double admissionMs = 0.0;
        double polygonMs = 0.0;
        double polygonSetupMs = 0.0;
        double polygonDensifyMs = 0.0;
        double polygonIntersectionMs = 0.0;
        double polygonCdtMs = 0.0;
        double polygonCdtSuperMs = 0.0;
        double polygonCdtPointMs = 0.0;
        double polygonCdtConstraintMs = 0.0;
        double polygonCdtExtractMs = 0.0;
        double polygonEcefMs = 0.0;
        double lineMs = 0.0;
        double extrusionMs = 0.0;
        double symbolMs = 0.0;
        size_t admittedFeatures = 0;
        size_t rejectedFeatures = 0;
        std::array<size_t, static_cast<size_t>(RejectionReason::Count)>
            rejectionCounts{};
        /// Diagnostics-only aggregation. Key = geometry type, class, subKey.
        /// It is never read by rendering and therefore cannot alter traversal
        /// or emitted geometry order.
        std::map<std::tuple<uint8_t, int, int>, size_t> rejectedIdentities;
        size_t polygonFeatures = 0;
        size_t lineFeatures = 0;
        size_t extrusionFeatures = 0;
        size_t symbolFeatures = 0;
        size_t rings = 0;
        size_t points = 0;
        size_t polygonInputPoints = 0;
        size_t polygonDensifiedPoints = 0;
        size_t polygonInitialConstraints = 0;
        size_t polygonFinalConstraints = 0;
        size_t polygonIntersectionPairs = 0;
        size_t polygonIntersectionCandidatePairs = 0;
        size_t polygonTriangles = 0;
        size_t polygonCdtPointTriangleTests = 0;
        size_t polygonCdtPointBadTriangles = 0;
        size_t polygonCdtConstraintEdgeTests = 0;
        size_t polygonCdtConstraintCrossTests = 0;
        size_t polygonCdtConstraintsAlreadyPresent = 0;
        size_t polygonCdtConstraintsInserted = 0;
        size_t polygonCdtPeakTriangles = 0;
        size_t polygonCdtPointCapacityGrowths = 0;
        size_t polygonCdtTriangleCapacityGrowths = 0;
        double slowestFeatureMs = 0.0;
        int slowestClassCode = 0;
        int slowestSubKey = 0;
        size_t slowestRings = 0;
        size_t slowestPoints = 0;
    } diagnostics;
    Vec3 origin = Vec3::zero();
    bool hasOrigin = false;
    std::vector<float> fillVerts;
    std::vector<uint32_t> fillIndices;
    std::vector<PaintRange> fillRanges;
    /// fill 顶点同序的 lon/lat/color 3f，用于 commit 与 terrain revision
    /// 重钳；官方 surface 不再停留在 worker 的椭球面占位高度。
    std::vector<float> fillClampSource;
    std::vector<float> lineVerts;
    std::vector<uint32_t> lineIndices;
    std::vector<PaintRange> lineRanges;
    /// 线 ribbon 的完整钳高源。worker 无地形采样时只产椭球面高度；
    /// 渲染线程 commit/revision 时按经纬度同源采样。
    std::vector<float> lineClampSource;
    /// 贴地(ClampToGround + 后端支持 stencil 分类)时,fill/line 改产
    /// 挤出体走双 pass 像素级贴合;此时上面的 fill/lineVerts 为空(两条路
    /// **互斥**,同时产出会让同一份内容画两遍)。
    VolumeCpuGroups fillVolumeGroups;
    VolumeCpuGroups lineVolumeGroups;
    /// V6 建筑挤出(pos3+normal3+color4=28B/顶点,相对 origin)。
    /// E3:与 store 路径的 BucketGpu::extrude* 同构,worker 全链镶嵌时
    /// 在这里携带,commitTileMesh 上传到 BucketGpu::extrude*。
    std::vector<float> extrudeVerts;
    std::vector<uint32_t> extrudeIndices;
    std::vector<PaintRange> extrudeRanges;
    /// 每最终建筑顶点 7f:lon/lat + relativeHeight + normal xyz + color。
    std::vector<float> extrudeClampSource;
    /// 点符号实例表(quad 定型留在渲染线程,见 TileSymbolCpu)。
    std::vector<TileSymbolCpu> symbols;

    bool empty() const {
        return fillIndices.empty() && lineIndices.empty() &&
               fillVolumeGroups.empty() && lineVolumeGroups.empty() &&
               extrudeIndices.empty() && symbols.empty();
    }
};

} // namespace earth_engine
