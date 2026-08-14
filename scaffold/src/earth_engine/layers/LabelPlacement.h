#pragma once

#include "../core/math/Mat4.h"
#include "../core/math/Vec3.h"
#include "../data/Feature.h"

#include <unordered_map>
#include <vector>

namespace earth_engine {

/// placement 候选(collect 段产物):每个可标注要素一条。
/// 碰撞盒是相对锚点屏幕投影位置的像素矩形(y 向上,与标签顶点
/// offsetPx 同一坐标约定),由镶嵌时的文字布局算出。
struct LabelCandidate {
    FeatureId featureId = kInvalidFeatureId;
    Vec3 anchorEcef;   ///< 绝对 ECEF(double,不减桶原点)
    float boxMinXPx = 0.0f;
    float boxMinYPx = 0.0f;
    float boxMaxXPx = 0.0f;
    float boxMaxYPx = 0.0f;
};

/// placement 诊断计数(每帧覆写)。
struct LabelPlacementStats {
    int candidates = 0;
    int culledProjection = 0;  ///< 相机背后 / 屏幕外
    int culledHorizon = 0;     ///< 椭球地平线遮挡(球背面)
    int collided = 0;          ///< 碰撞落选
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
/// - 排序:选中要素提权(编辑联动)> 3D 距离近者 > featureId——全部
///   确定性,保证帧间稳定不闪。
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
