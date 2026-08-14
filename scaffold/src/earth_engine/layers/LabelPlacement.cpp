#include "LabelPlacement.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace earth_engine {

namespace {

/// place 段中间态:投影/剔除通过的候选。
struct PlacedBox {
    int candidateIndex = -1;
    float minX = 0, minY = 0, maxX = 0, maxY = 0;  ///< 屏幕 px(含 padding)
    double distanceSq = 0.0;
    float horizonFade = 1.0f;
};

/// 屏幕均匀网格碰撞索引(设计 §8.2:collision grid 保留)。
/// 盒可部分出屏,格坐标 clamp 进视口范围。
class CollisionGrid {
public:
    CollisionGrid(float viewportW, float viewportH, float cellPx)
        : cellPx_(cellPx),
          cols_(std::max(1, static_cast<int>(std::ceil(viewportW / cellPx)))),
          rows_(std::max(1, static_cast<int>(std::ceil(viewportH / cellPx)))),
          cells_(static_cast<size_t>(cols_) * rows_) {}

    bool collides(const PlacedBox& box) const {
        int c0, c1, r0, r1;
        cellRange(box, c0, c1, r0, r1);
        for (int r = r0; r <= r1; ++r) {
            for (int c = c0; c <= c1; ++c) {
                for (int idx : cells_[static_cast<size_t>(r) * cols_ + c]) {
                    const PlacedBox& other = accepted_[idx];
                    if (box.minX < other.maxX && box.maxX > other.minX &&
                        box.minY < other.maxY && box.maxY > other.minY) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    void insert(const PlacedBox& box) {
        const int idx = static_cast<int>(accepted_.size());
        accepted_.push_back(box);
        int c0, c1, r0, r1;
        cellRange(box, c0, c1, r0, r1);
        for (int r = r0; r <= r1; ++r) {
            for (int c = c0; c <= c1; ++c) {
                cells_[static_cast<size_t>(r) * cols_ + c].push_back(idx);
            }
        }
    }

private:
    void cellRange(const PlacedBox& box,
                   int& c0, int& c1, int& r0, int& r1) const {
        c0 = std::clamp(static_cast<int>(box.minX / cellPx_), 0, cols_ - 1);
        c1 = std::clamp(static_cast<int>(box.maxX / cellPx_), 0, cols_ - 1);
        r0 = std::clamp(static_cast<int>(box.minY / cellPx_), 0, rows_ - 1);
        r1 = std::clamp(static_cast<int>(box.maxY / cellPx_), 0, rows_ - 1);
    }

    float cellPx_;
    int cols_;
    int rows_;
    std::vector<std::vector<int>> cells_;
    std::vector<PlacedBox> accepted_;
};

} // namespace

bool LabelPlacement::update(const FrameInput& in,
                            const std::vector<LabelCandidate>& candidates) {
    stats_ = LabelPlacementStats{};
    stats_.candidates = static_cast<int>(candidates.size());

    const double vpW = static_cast<double>(in.viewportWidthPx);
    const double vpH = static_cast<double>(in.viewportHeightPx);
    const glm::dmat4& viewProj = in.viewProj.raw();

    // 椭球缩放空间地平线遮挡预备量(cesium EllipsoidalOccluder 同款):
    // 相机缩放坐标 cv,vh² = |cv|²-1 = 相机到地平圆切点距离平方(缩放空间)。
    // 相机在椭球内/面上(vh²<=0)时无遮挡概念,全部可见。
    const glm::dvec3 radii(in.ellipsoidRadii.x(), in.ellipsoidRadii.y(),
                           in.ellipsoidRadii.z());
    const glm::dvec3 cv(in.cameraEcef.x() / radii.x,
                        in.cameraEcef.y() / radii.y,
                        in.cameraEcef.z() / radii.z);
    const double vhMagSq = glm::dot(cv, cv) - 1.0;

    // ---- place:投影 + 剔除 ----
    std::vector<PlacedBox> boxes;
    boxes.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        const LabelCandidate& cand = candidates[i];
        const glm::dvec4 cp =
            viewProj * glm::dvec4(cand.anchorEcef.x(), cand.anchorEcef.y(),
                                  cand.anchorEcef.z(), 1.0);
        if (cp.w <= 0.0) {
            ++stats_.culledProjection;
            continue;
        }
        const double sx = (cp.x / cp.w * 0.5 + 0.5) * vpW;
        const double sy = (cp.y / cp.w * 0.5 + 0.5) * vpH;

        PlacedBox box;
        box.candidateIndex = static_cast<int>(i);
        box.minX = static_cast<float>(sx) + cand.boxMinXPx - kCollisionPaddingPx;
        box.maxX = static_cast<float>(sx) + cand.boxMaxXPx + kCollisionPaddingPx;
        box.minY = static_cast<float>(sy) + cand.boxMinYPx - kCollisionPaddingPx;
        box.maxY = static_cast<float>(sy) + cand.boxMaxYPx + kCollisionPaddingPx;
        if (box.maxX < 0.0f || box.minX > vpW ||
            box.maxY < 0.0f || box.minY > vpH) {
            ++stats_.culledProjection;
            continue;
        }

        // 地平线遮挡 + 近地平线 fade(缩放空间):余量
        // margin = vh² - vt·(-cv) = |cv|·cosθ - 1(θ = 锚点对相机中轴的
        // 地心角),地平圆处恰为 0、天底处最大 |cv|-1。margin ≤ 0 → 遮挡
        // (表面锚点在此即越过地平圆;高出表面的"探头"点也已在视觉极限,
        // 一并隐藏——cesium 双条件遮挡的第二条件对表面点退化,不适合做
        // fade 度量)。归一化 band = kHorizonFadeBand × (|cv|-1) 随相机
        // 高度缩放,落入者线性渐隐 → 标签滑向地平线时不硬弹。
        if (vhMagSq > 0.0) {
            const glm::dvec3 p(cand.anchorEcef.x() / radii.x,
                               cand.anchorEcef.y() / radii.y,
                               cand.anchorEcef.z() / radii.z);
            const glm::dvec3 vt = p - cv;
            const double vtDotVc = -glm::dot(vt, cv);
            const double margin = vhMagSq - vtDotVc;
            if (margin <= 0.0) {
                ++stats_.culledHorizon;
                continue;
            }
            const double maxMargin = std::sqrt(vhMagSq + 1.0) - 1.0;
            const double band = kHorizonFadeBand * maxMargin;
            if (band > 0.0 && margin < band) {
                box.horizonFade = static_cast<float>(margin / band);
            }
        }

        const glm::dvec3 toAnchor(cand.anchorEcef.x() - in.cameraEcef.x(),
                                  cand.anchorEcef.y() - in.cameraEcef.y(),
                                  cand.anchorEcef.z() - in.cameraEcef.z());
        box.distanceSq = glm::dot(toAnchor, toAnchor);
        boxes.push_back(box);
    }

    // ---- 排序:提权 > 距离近 > featureId(确定性,帧间稳定) ----
    std::sort(boxes.begin(), boxes.end(),
              [&](const PlacedBox& a, const PlacedBox& b) {
                  const FeatureId fa = candidates[a.candidateIndex].featureId;
                  const FeatureId fb = candidates[b.candidateIndex].featureId;
                  const bool pa = fa == priorityFeature_;
                  const bool pb = fb == priorityFeature_;
                  if (pa != pb) return pa;
                  if (a.distanceSq != b.distanceSq)
                      return a.distanceSq < b.distanceSq;
                  return fa < fb;
              });

    // ---- 碰撞入格 + commit 目标 ----
    for (auto& [id, fade] : fades_) fade.touched = false;

    CollisionGrid grid(static_cast<float>(vpW), static_cast<float>(vpH),
                       kGridCellPx);
    std::unordered_map<FeatureId, float> targets;
    targets.reserve(boxes.size());
    for (const PlacedBox& box : boxes) {
        const FeatureId fid = candidates[box.candidateIndex].featureId;
        if (grid.collides(box)) {
            ++stats_.collided;
            targets[fid] = 0.0f;
            continue;
        }
        grid.insert(box);
        ++stats_.placed;
        targets[fid] = box.horizonFade;
    }

    // ---- fade 状态机:current 朝 target 以 kFadeSeconds 满程速率推进 ----
    bool changed = false;
    const float step = kFadeSeconds > 0.0
        ? static_cast<float>(in.deltaSeconds / kFadeSeconds)
        : 1.0f;
    for (const LabelCandidate& cand : candidates) {
        FadeState& fade = fades_[cand.featureId];
        fade.touched = true;
        const auto it = targets.find(cand.featureId);
        fade.target = it != targets.end() ? it->second : 0.0f;
        if (fade.current != fade.target) {
            const float delta = fade.target - fade.current;
            const float move = std::min(std::abs(delta), step);
            if (move > 0.0f) {
                fade.current += delta > 0.0f ? move : -move;
                changed = true;
            }
        }
    }

    // 清扫:候选集里消失的要素(删除/桶重镶换代)丢状态,重现时从 0 fade。
    for (auto it = fades_.begin(); it != fades_.end();) {
        it = it->second.touched ? std::next(it) : fades_.erase(it);
    }
    return changed;
}

bool LabelPlacement::advanceFades(double deltaSeconds) {
    bool changed = false;
    const float step = kFadeSeconds > 0.0
        ? static_cast<float>(deltaSeconds / kFadeSeconds)
        : 1.0f;
    for (auto& [id, fade] : fades_) {
        if (fade.current == fade.target) continue;
        const float delta = fade.target - fade.current;
        const float move = std::min(std::abs(delta), step);
        if (move > 0.0f) {
            fade.current += delta > 0.0f ? move : -move;
            changed = true;
        }
    }
    return changed;
}

float LabelPlacement::opacity(FeatureId id) const {
    const auto it = fades_.find(id);
    return it != fades_.end() ? it->second.current : 0.0f;
}

} // namespace earth_engine
