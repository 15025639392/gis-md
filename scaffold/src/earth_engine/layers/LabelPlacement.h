#pragma once

#include "../core/math/Mat4.h"
#include "../core/math/Vec3.h"
#include "../data/Feature.h"

#include <unordered_map>
#include <vector>
#include <array>

namespace earth_engine {

struct LabelCollisionPart {
    Vec3 anchorEcef;
    Vec3 tangentEcef;
    float minXPx = 0.0f;
    float minYPx = 0.0f;
    float maxXPx = 0.0f;
    float maxYPx = 0.0f;
};

/// placement 候选(collect 段产物):每个可标注要素一条。
/// 碰撞盒是相对锚点屏幕投影位置的像素矩形(y 向上,与标签顶点
/// offsetPx 同一坐标约定),由镶嵌时的文字布局算出。
struct LabelCandidate {
    FeatureId featureId = kInvalidFeatureId;
    int rank = 6;       ///< 数据侧重要度；更小者优先进入碰撞网格
    /// Sealed AMap placement uses provider rank, then the worker's numeric
    /// Util.stamp id in descending order. Zero means generic provider; generic
    /// layers retain the engine's distance/id tie-break contract.
    uint64_t officialInsertionOrder = 0;
    uint32_t officialFragmentOrder = 0;
    Vec3 anchorEcef;   ///< 绝对 ECEF(double,不减桶原点)
    Vec3 tangentEcef;  ///< 方向参考；与 anchor 相同表示水平标签
    float boxMinXPx = 0.0f;
    float boxMinYPx = 0.0f;
    float boxMaxXPx = 0.0f;
    float boxMaxYPx = 0.0f;
    bool hasSecondaryBox = false;
    float secondaryBoxMinXPx = 0.0f;
    float secondaryBoxMinYPx = 0.0f;
    float secondaryBoxMaxXPx = 0.0f;
    float secondaryBoxMaxYPx = 0.0f;
    uint64_t repeatGroup = 0;       ///< 0=不启用同名重复间距
    float repeatDistancePx = 0.0f;  ///< 同组已放置锚点的最小屏幕距离
    float angleRad = 0.0f;          ///< 局部 east/north 切线角
    float paddingXPx = 0.0f;        ///< 候选专属水平碰撞留白
    float paddingYPx = 0.0f;        ///< 候选专属垂直碰撞留白
    /// Official `canCovered`: the candidate does not search for and reject
    /// lower-priority labels, but it may still be rejected by a previously
    /// accepted higher-priority ordinary label. It never enters the accepted
    /// collision grid, so lower-priority labels are not blocked by it.
    bool officialCanCovered = false;
    /// Along-path labels preserve the actual per-glyph anchors and tangents.
    /// When non-empty these parts are the sole text collision geometry; the
    /// centered primary box is not used as a compatibility fallback.
    std::vector<LabelCollisionPart> collisionParts;
};

/// placement 诊断计数(每帧覆写)。
struct LabelPlacementStats {
    int candidates = 0;
    int culledProjection = 0;  ///< 相机背后 / 屏幕外
    int culledHorizon = 0;     ///< 椭球地平线遮挡(球背面)
    int collided = 0;          ///< 碰撞落选
    int repeated = 0;          ///< 同 repeatGroup 距离过近落选
    int placed = 0;            ///< 本帧目标可见
};

/// 标签避让 placement(矢量 P5 下半场,设计 §8.2)。
///
/// 三段:collect(调用方遍历桶收集 LabelCandidate)→ place(本类:投影
/// →视锥/地平线剔除→priority 排序→屏幕均匀网格碰撞)→ commit(目标
/// 透明度 + ~300ms fade 状态机,调用方按 opacity() 回写顶点流)。
///
/// 球面化改造点(相对 maplibre 屏幕空间 placement):
/// - 地平线遮挡:椭球缩放空间标准遮挡测试(cesium EllipsoidalOccluder
///   同款公式),球背面标签剔除;近地平线按遮挡比 fade band 渐隐。
/// - 尺寸按 3D 距离:每候选独立投影自身锚点,碰撞盒即真实屏幕盒
///   (maplibre 的"按瓦片中心统一缩放"问题在此结构下天然不存在)。
/// - 排序:选中要素提权(编辑联动)> 数据 rank > 3D 距离近者 >
///   featureId——全部确定性,保证帧间稳定不闪。
///
/// 逐帧调用(设计定"逐帧避让");demo 万级以下规模逐帧全量投影成本
/// 微不足道,更大规模再谈增量。纯 CPU、无 GL 依赖,host 可单测。
///
/// 已知残余(明记,不是漏想):地形凸包遮挡(山背后的标签仍显示)需
/// 深度回读或沿视线地形采样,首版不做;跨源去重地理 key 属 MVT(P4)。
class LabelPlacement {
public:
    static constexpr double kFadeSeconds = 0.3;
    static constexpr float kCollisionPaddingPx = 2.0f;
    /// 近地平线 fade band:缩放空间可见余量占天底最大余量的比例,
    /// 落入 [0, band) 线性渐隐(见 .cpp margin 注释)。
    static constexpr double kHorizonFadeBand = 0.12;
    static constexpr float kGridCellPx = 64.0f;

    static std::array<double, 2> readableScreenDirection(double dx,
                                                         double dy);
    static std::array<double, 4> rotatedScreenBounds(
        float minX, float minY, float maxX, float maxY,
        double directionX, double directionY);

    /// 热点③ 视锥预剔除:锚点投影后保守判"整盒必在屏外"。盒各角到锚点最大
    /// 距离 + padding = 外接圆半径,锚点越出视口余量即整盒(含 secondary 盒)
    /// 必在屏外;旋转是绕锚点刚体变换,角距不变,外接圆恒覆盖旋转盒。相机背后
    /// 也返回 true。**collisionParts 非空(沿线标签,部件锚点独立于主锚点)时
    /// 本判据不可用,调用方负责跳过**。collect(省建候选)与 update(省投影)
    /// 共用,保证两处剔除口径一致。
    static bool boxFullyOffscreenScreen(
        const Vec3& anchorEcef, const Mat4& viewProj,
        double viewportW, double viewportH,
        float boxMinXPx, float boxMinYPx, float boxMaxXPx, float boxMaxYPx,
        bool hasSecondary, float sMinXPx, float sMinYPx, float sMaxXPx,
        float sMaxYPx, float paddingXPx, float paddingYPx);

    struct FrameInput {
        Mat4 viewProj;          ///< double viewProjection(绝对 ECEF)
        Vec3 cameraEcef;
        Vec3 ellipsoidRadii;
        int viewportWidthPx = 0;
        int viewportHeightPx = 0;
        double deltaSeconds = 0.0;
    };

    /// 编辑联动:选中要素标签提权(placement 最先入格)。kInvalid = 清除。
    void setPriorityFeature(FeatureId id) { priorityFeature_ = id; }
    FeatureId priorityFeature() const { return priorityFeature_; }

    /// 全量 place+commit(投影→剔除→碰撞→定 target + fade 步进)。
    /// 返回 true = 任一标签 opacity 有变化(调用方据此决定是否回写顶点
    /// 流重传)。**符号刀D 起按 ~300ms 节流调用**(见调用方),两次之间
    /// 每帧只走 advanceFades —— maplibre 同款拆分:碰撞判定贵在全量
    /// 投影,渐变收敛必须逐帧平滑,二者节奏本就不同。
    bool update(const FrameInput& in,
                const std::vector<LabelCandidate>& candidates);

    /// 只推进 fade(current → 既有 target),不投影不碰撞不改 target、
    /// 不清扫消失要素。节流间隙每帧调用,保 ~300ms 渐变逐帧平滑。
    /// 返回 true = 任一 opacity 有变化。
    bool advanceFades(double deltaSeconds);

    /// 当前渐变后透明度(0 = 隐藏或未知要素)。
    float opacity(FeatureId id) const;

    /// 该 id 的 fade 目标值(0 = 隐藏或未知要素)。**纯查询,不推进**。
    /// 与 opacity() 的 current 配对读出在途方向(七态 dump 用)。
    float fadeTarget(FeatureId id) const {
        auto it = fades_.find(id);
        return it == fades_.end() ? 0.0f : it->second.target;
    }

    /// 是否还有 fade 未收敛(current != target)。**纯查询,不推进**。
    /// V27:帧循环据此判"还得再出帧"——fade 靠逐帧 advanceFades 推进,而
    /// 引擎/应用的收敛判据本来看不见它,冷启动瓦片一加载完就停帧,标注 fade
    /// 冻在半程(~0.15 透明度)= POI 首现必须缩放催化的真根因。
    bool hasPendingFades() const {
        for (const auto& [id, fade] : fades_) {
            if (fade.current != fade.target) return true;
        }
        return false;
    }

    const LabelPlacementStats& stats() const { return stats_; }

private:
    struct FadeState {
        float current = 0.0f;
        float target = 0.0f;
        bool touched = false;  ///< 本帧候选集里出现过(清扫标记)
    };

    std::unordered_map<FeatureId, FadeState> fades_;
    FeatureId priorityFeature_ = kInvalidFeatureId;
    LabelPlacementStats stats_;
};

} // namespace earth_engine
