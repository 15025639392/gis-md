#include "LabelPlacement.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace earth_engine {

namespace {

/// place 段中间态:投影/剔除通过的候选。
struct PlacedBox {
    struct Rect { float minX, minY, maxX, maxY; };
    int candidateIndex = -1;
    float minX = 0, minY = 0, maxX = 0, maxY = 0;  ///< 屏幕 px(含 padding)
    double distanceSq = 0.0;
    float horizonFade = 1.0f;
    float anchorX = 0.0f;
    float anchorY = 0.0f;
    bool hasSecondary = false;
    float secondaryMinX = 0, secondaryMinY = 0;
    float secondaryMaxX = 0, secondaryMaxY = 0;
    std::vector<Rect> collisionParts;
};

bool placedBoxesOverlap(const PlacedBox& a, const PlacedBox& b) {
    const auto overlaps = [](float aMinX, float aMinY, float aMaxX,
                             float aMaxY, float bMinX, float bMinY,
                             float bMaxX, float bMaxY) {
        return aMinX < bMaxX && aMaxX > bMinX &&
               aMinY < bMaxY && aMaxY > bMinY;
    };
    if (!a.collisionParts.empty() || !b.collisionParts.empty()) {
        const auto visit = [&](const PlacedBox& box, const auto& fn) {
            if (!box.collisionParts.empty()) {
                for (const auto& rect : box.collisionParts) fn(rect);
            } else {
                fn(PlacedBox::Rect{box.minX, box.minY, box.maxX, box.maxY});
                if (box.hasSecondary) {
                    fn(PlacedBox::Rect{box.secondaryMinX, box.secondaryMinY,
                                       box.secondaryMaxX, box.secondaryMaxY});
                }
            }
        };
        bool hit = false;
        visit(a, [&](const PlacedBox::Rect& ar) {
            visit(b, [&](const PlacedBox::Rect& br) {
                hit = hit || overlaps(ar.minX, ar.minY, ar.maxX, ar.maxY,
                                      br.minX, br.minY, br.maxX, br.maxY);
            });
        });
        return hit;
    }
    return overlaps(a.minX, a.minY, a.maxX, a.maxY,
                    b.minX, b.minY, b.maxX, b.maxY) ||
           (a.hasSecondary && overlaps(
               a.secondaryMinX, a.secondaryMinY, a.secondaryMaxX,
               a.secondaryMaxY, b.minX, b.minY, b.maxX, b.maxY)) ||
           (b.hasSecondary && overlaps(
               a.minX, a.minY, a.maxX, a.maxY, b.secondaryMinX,
               b.secondaryMinY, b.secondaryMaxX, b.secondaryMaxY)) ||
           (a.hasSecondary && b.hasSecondary && overlaps(
               a.secondaryMinX, a.secondaryMinY, a.secondaryMaxX,
               a.secondaryMaxY, b.secondaryMinX, b.secondaryMinY,
               b.secondaryMaxX, b.secondaryMaxY));
}

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
                    if (placedBoxesOverlap(box, other)) {
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
        float minX = box.hasSecondary
            ? std::min(box.minX, box.secondaryMinX) : box.minX;
        float maxX = box.hasSecondary
            ? std::max(box.maxX, box.secondaryMaxX) : box.maxX;
        float minY = box.hasSecondary
            ? std::min(box.minY, box.secondaryMinY) : box.minY;
        float maxY = box.hasSecondary
            ? std::max(box.maxY, box.secondaryMaxY) : box.maxY;
        if (!box.collisionParts.empty()) {
            minX = minY = std::numeric_limits<float>::infinity();
            maxX = maxY = -std::numeric_limits<float>::infinity();
            for (const auto& rect : box.collisionParts) {
                minX = std::min(minX, rect.minX);
                minY = std::min(minY, rect.minY);
                maxX = std::max(maxX, rect.maxX);
                maxY = std::max(maxY, rect.maxY);
            }
        }
        c0 = std::clamp(static_cast<int>(minX / cellPx_), 0, cols_ - 1);
        c1 = std::clamp(static_cast<int>(maxX / cellPx_), 0, cols_ - 1);
        r0 = std::clamp(static_cast<int>(minY / cellPx_), 0, rows_ - 1);
        r1 = std::clamp(static_cast<int>(maxY / cellPx_), 0, rows_ - 1);
    }

    float cellPx_;
    int cols_;
    int rows_;
    std::vector<std::vector<int>> cells_;
    std::vector<PlacedBox> accepted_;
};

} // namespace

std::array<double, 2> LabelPlacement::readableScreenDirection(double dx,
                                                               double dy) {
    const double len = std::hypot(dx, dy);
    if (len <= 1e-6) return {1.0, 0.0};
    dx /= len;
    dy /= len;
    if (dx < 0.0) {
        dx = -dx;
        dy = -dy;
    }
    return {dx, dy};
}

std::array<double, 4> LabelPlacement::rotatedScreenBounds(
    float minX, float minY, float maxX, float maxY,
    double directionX, double directionY) {
    const auto dir = readableScreenDirection(directionX, directionY);
    const double normalX = -dir[1];
    const double normalY = dir[0];
    const double corners[4][2] = {
        {minX, minY}, {maxX, minY}, {maxX, maxY}, {minX, maxY}};
    std::array<double, 4> bounds{
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    for (const auto& corner : corners) {
        const double x = dir[0] * corner[0] + normalX * corner[1];
        const double y = dir[1] * corner[0] + normalY * corner[1];
        bounds[0] = std::min(bounds[0], x);
        bounds[1] = std::min(bounds[1], y);
        bounds[2] = std::max(bounds[2], x);
        bounds[3] = std::max(bounds[3], y);
    }
    return bounds;
}

bool LabelPlacement::boxFullyOffscreenScreen(
    const Vec3& anchorEcef, const Mat4& viewProj,
    double viewportW, double viewportH,
    float boxMinXPx, float boxMinYPx, float boxMaxXPx, float boxMaxYPx,
    bool hasSecondary, float sMinXPx, float sMinYPx, float sMaxXPx,
    float sMaxYPx, float paddingXPx, float paddingYPx) {
    const glm::dvec4 cp = viewProj.raw() * glm::dvec4(
        anchorEcef.x(), anchorEcef.y(), anchorEcef.z(), 1.0);
    if (cp.w <= 0.0) return true;  // 相机背后(与 update 的 cp.w<=0 剔除一致)
    const double sx = (cp.x / cp.w * 0.5 + 0.5) * viewportW;
    const double sy = (cp.y / cp.w * 0.5 + 0.5) * viewportH;
    float maxR = std::max({std::abs(boxMinXPx), std::abs(boxMaxXPx),
                           std::abs(boxMinYPx), std::abs(boxMaxYPx)});
    if (hasSecondary) {
        maxR = std::max({maxR, std::abs(sMinXPx), std::abs(sMinYPx),
                         std::abs(sMaxXPx), std::abs(sMaxYPx)});
    }
    maxR += std::max(0.0f, std::max(paddingXPx, paddingYPx));
    return sx < -maxR || sx > viewportW + maxR ||
           sy < -maxR || sy > viewportH + maxR;
}

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

        // [热点③] 视锥预剔除:锚点投影后立刻用盒最大外接半径做保守剔除,
        // 跳过切线/沿线碰撞部件投影/旋转包围盒等昂贵计算(热点来源 —— V27
        // 实测候选 98.7% 是驻留远瓦的屏外候选,此前它们算完整盒才被 311 行
        // 剔除,纯浪费)。沿线标签(collisionParts 非空)部件锚点独立于主锚点,
        // 不可用主锚点外接圆界定 → 保留原路径。
        if (cand.collisionParts.empty() &&
            boxFullyOffscreenScreen(
                cand.anchorEcef, in.viewProj, vpW, vpH,
                cand.boxMinXPx, cand.boxMinYPx, cand.boxMaxXPx,
                cand.boxMaxYPx, cand.hasSecondaryBox,
                cand.secondaryBoxMinXPx, cand.secondaryBoxMinYPx,
                cand.secondaryBoxMaxXPx, cand.secondaryBoxMaxYPx,
                cand.paddingXPx, cand.paddingYPx)) {
            ++stats_.culledProjection;
            continue;
        }

        double dirX = 1.0;
        double dirY = 0.0;
        if (cand.tangentEcef.lengthSquared() > 0.0 &&
            (cand.tangentEcef - cand.anchorEcef).lengthSquared() > 1e-12) {
            const glm::dvec4 tp = viewProj * glm::dvec4(
                cand.tangentEcef.x(), cand.tangentEcef.y(),
                cand.tangentEcef.z(), 1.0);
            if (tp.w > 0.0) {
                const double tx = (tp.x / tp.w * 0.5 + 0.5) * vpW;
                const double ty = (tp.y / tp.w * 0.5 + 0.5) * vpH;
                const double dx = tx - sx;
                const double dy = ty - sy;
                const auto dir = readableScreenDirection(dx, dy);
                dirX = dir[0];
                dirY = dir[1];
            }
        }
        const auto bounds = rotatedScreenBounds(
            cand.boxMinXPx, cand.boxMinYPx, cand.boxMaxXPx,
            cand.boxMaxYPx, dirX, dirY);

        PlacedBox box;
        box.candidateIndex = static_cast<int>(i);
        box.anchorX = static_cast<float>(sx);
        box.anchorY = static_cast<float>(sy);
        const float paddingX = std::max(0.0f, cand.paddingXPx);
        const float paddingY = std::max(0.0f, cand.paddingYPx);
        box.minX = static_cast<float>(sx + bounds[0]) - paddingX;
        box.maxX = static_cast<float>(sx + bounds[2]) + paddingX;
        box.minY = static_cast<float>(sy + bounds[1]) - paddingY;
        box.maxY = static_cast<float>(sy + bounds[3]) + paddingY;
        for (const LabelCollisionPart& part : cand.collisionParts) {
            const glm::dvec4 pp = viewProj * glm::dvec4(
                part.anchorEcef.x(), part.anchorEcef.y(),
                part.anchorEcef.z(), 1.0);
            if (pp.w <= 0.0) continue;
            const double px = (pp.x / pp.w * 0.5 + 0.5) * vpW;
            const double py = (pp.y / pp.w * 0.5 + 0.5) * vpH;
            double partDirX = 1.0;
            double partDirY = 0.0;
            if ((part.tangentEcef - part.anchorEcef).lengthSquared() > 1e-12) {
                const glm::dvec4 pt = viewProj * glm::dvec4(
                    part.tangentEcef.x(), part.tangentEcef.y(),
                    part.tangentEcef.z(), 1.0);
                if (pt.w > 0.0) {
                    const double tx = (pt.x / pt.w * 0.5 + 0.5) * vpW;
                    const double ty = (pt.y / pt.w * 0.5 + 0.5) * vpH;
                    const auto partDir = readableScreenDirection(tx - px,
                                                                 ty - py);
                    partDirX = partDir[0];
                    partDirY = partDir[1];
                }
            }
            const auto partBounds = rotatedScreenBounds(
                part.minXPx, part.minYPx, part.maxXPx, part.maxYPx,
                partDirX, partDirY);
            box.collisionParts.push_back(PlacedBox::Rect{
                static_cast<float>(px + partBounds[0]) - paddingX,
                static_cast<float>(py + partBounds[1]) - paddingY,
                static_cast<float>(px + partBounds[2]) + paddingX,
                static_cast<float>(py + partBounds[3]) + paddingY});
        }
        if (cand.hasSecondaryBox) {
            const auto secondary = rotatedScreenBounds(
                cand.secondaryBoxMinXPx, cand.secondaryBoxMinYPx,
                cand.secondaryBoxMaxXPx, cand.secondaryBoxMaxYPx,
                dirX, dirY);
            box.hasSecondary = true;
            box.secondaryMinX = static_cast<float>(sx + secondary[0]);
            box.secondaryMaxX = static_cast<float>(sx + secondary[2]);
            box.secondaryMinY = static_cast<float>(sy + secondary[1]);
            box.secondaryMaxY = static_cast<float>(sy + secondary[3]);
        }
        // Amap-style continuous panning allows a label to enter/leave the
        // viewport progressively.  Cull only when the complete padded box is
        // outside; the framebuffer clips the visible fragment naturally.
        float combinedMinX = box.hasSecondary
            ? std::min(box.minX, box.secondaryMinX) : box.minX;
        float combinedMaxX = box.hasSecondary
            ? std::max(box.maxX, box.secondaryMaxX) : box.maxX;
        float combinedMinY = box.hasSecondary
            ? std::min(box.minY, box.secondaryMinY) : box.minY;
        float combinedMaxY = box.hasSecondary
            ? std::max(box.maxY, box.secondaryMaxY) : box.maxY;
        if (!box.collisionParts.empty()) {
            combinedMinX = combinedMinY =
                std::numeric_limits<float>::infinity();
            combinedMaxX = combinedMaxY =
                -std::numeric_limits<float>::infinity();
            for (const auto& rect : box.collisionParts) {
                combinedMinX = std::min(combinedMinX, rect.minX);
                combinedMinY = std::min(combinedMinY, rect.minY);
                combinedMaxX = std::max(combinedMaxX, rect.maxX);
                combinedMaxY = std::max(combinedMaxY, rect.maxY);
            }
        }
        if (combinedMaxX < 0.0f || combinedMinX > vpW ||
            combinedMaxY < 0.0f || combinedMinY > vpH) {
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

    // ---- 排序 ----
    // Official AMap: worker Util.stamp ids increase monotonically; the main
    // thread enumerates each numeric-key rank object ascending and then walks
    // it backwards.  Carry that id explicitly: candidate vector position is
    // not equivalent because tile buckets live in an unordered_map. Generic
    // layers keep their established camera-distance/id policy.
    std::sort(boxes.begin(), boxes.end(),
              [&](const PlacedBox& a, const PlacedBox& b) {
                  const FeatureId fa = candidates[a.candidateIndex].featureId;
                  const FeatureId fb = candidates[b.candidateIndex].featureId;
                  const bool pa = fa == priorityFeature_;
                  const bool pb = fb == priorityFeature_;
                  if (pa != pb) return pa;
                  const int ra = candidates[a.candidateIndex].rank;
                  const int rb = candidates[b.candidateIndex].rank;
                  if (ra != rb) return ra < rb;
                  const uint64_t officialA =
                      candidates[a.candidateIndex].officialInsertionOrder;
                  const uint64_t officialB =
                      candidates[b.candidateIndex].officialInsertionOrder;
                  if (officialA != 0 || officialB != 0) {
                      if (officialA != officialB) return officialA > officialB;
                      const uint32_t fragmentA =
                          candidates[a.candidateIndex].officialFragmentOrder;
                      const uint32_t fragmentB =
                          candidates[b.candidateIndex].officialFragmentOrder;
                      if (fragmentA != fragmentB) return fragmentA > fragmentB;
                  }
                  if (a.distanceSq != b.distanceSq)
                      return a.distanceSq < b.distanceSq;
                  return fa < fb;
              });

    // ---- 碰撞入格 + commit 目标 ----
    for (auto& [id, fade] : fades_) fade.touched = false;

    CollisionGrid grid(static_cast<float>(vpW), static_cast<float>(vpH),
                       kGridCellPx);
    std::unordered_map<uint64_t, std::vector<std::pair<float, float>>>
        repeatAnchors;
    std::unordered_map<FeatureId, float> targets;
    targets.reserve(boxes.size());
    for (const PlacedBox& box : boxes) {
        const LabelCandidate& candidate = candidates[box.candidateIndex];
        const FeatureId fid = candidate.featureId;
        if (candidate.repeatGroup != 0 &&
            candidate.repeatDistancePx > 0.0f) {
            const float minDistanceSq = candidate.repeatDistancePx *
                                        candidate.repeatDistancePx;
            bool tooClose = false;
            const auto it = repeatAnchors.find(candidate.repeatGroup);
            if (it != repeatAnchors.end()) {
                for (const auto& anchor : it->second) {
                    const float dx = box.anchorX - anchor.first;
                    const float dy = box.anchorY - anchor.second;
                    if (dx * dx + dy * dy < minDistanceSq) {
                        tooClose = true;
                        break;
                    }
                }
            }
            if (tooClose) {
                ++stats_.repeated;
                targets[fid] = 0.0f;
                continue;
            }
        }
        if (candidate.officialCanCovered) {
            // Official Ff/Ef ordering: every box exists in the search tree,
            // so a higher-priority ordinary item can mark canCovered hidden
            // before its turn. On its own turn canCovered skips its rd/hd
            // searches, therefore it rejects nobody. Our priority-ordered
            // accepted grid is equivalent: query higher-priority ordinary
            // boxes, but never insert this candidate for lower priorities.
            if (grid.collides(box)) {
                ++stats_.collided;
                targets[fid] = 0.0f;
                continue;
            }
            ++stats_.placed;
            targets[fid] = box.horizonFade;
            continue;
        }
        if (grid.collides(box)) {
            ++stats_.collided;
            targets[fid] = 0.0f;
            continue;
        }
        grid.insert(box);
        if (candidate.repeatGroup != 0 &&
            candidate.repeatDistancePx > 0.0f) {
            repeatAnchors[candidate.repeatGroup].push_back(
                {box.anchorX, box.anchorY});
        }
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
