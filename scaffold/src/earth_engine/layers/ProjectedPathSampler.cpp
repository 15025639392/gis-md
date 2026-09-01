#include "ProjectedPathSampler.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace earth_engine {

std::vector<ProjectedPathSampler> ProjectedPathSampler::createCandidates(
    const std::vector<Vec3>& pathEcef, const Mat4& viewProjection,
    double viewportWidthPx, double viewportHeightPx,
    double officialFontSizePx, double displayZoom,
    double providerPixelRatio) {
    if (pathEcef.size() < 2 || !(viewportWidthPx > 0.0) ||
        !(viewportHeightPx > 0.0) || !(officialFontSizePx > 0.0) ||
        !std::isfinite(displayZoom) || !(providerPixelRatio > 0.0) ||
        !std::isfinite(providerPixelRatio)) return {};
    struct Point {
        Vec3 ecef;
        glm::dvec2 screen;
        double clipW;
    };
    std::vector<Point> projected;
    projected.reserve(pathEcef.size());
    for (const Vec3& point : pathEcef) {
        const glm::dvec4 clip = viewProjection.raw() * glm::dvec4(
            point.x(), point.y(), point.z(), 1.0);
        if (!(clip.w > 0.0) || !std::isfinite(clip.w)) return {};
        const double x = (clip.x / clip.w * 0.5 + 0.5) * viewportWidthPx;
        const double y = (clip.y / clip.w * 0.5 + 0.5) * viewportHeightPx;
        if (!std::isfinite(x) || !std::isfinite(y)) return {};
        projected.push_back(Point{point, {x, y}, clip.w});
    }

    // AMap LabelLine.Zp first derives screen-space candidate paths. It ends a
    // candidate at turns over PI/10, at reading-direction reversals, and at
    // segments shorter than 0.6 * (fontSize * 1.3). The previous local path
    // treated the complete source line as one rail and smoothed across those
    // rejected corners, which is why glyphs could visibly leave the road.
    constexpr double kMaximumTurn = M_PI / 10.0;
    const double segmentUnit = officialFontSizePx * 1.3;
    const double activationLength = providerPixelRatio *
        (displayZoom < 12.0 ? 10.0 : 70.0);
    std::vector<std::vector<Point>> candidates(1);
    std::optional<Point> previousStart;
    std::optional<bool> readingReverse;
    double accumulated = 0.0;
    double previousAccumulated = 0.0;
    for (size_t i = 1; i < projected.size(); ++i) {
        Point start = projected[i - 1];
        const Point& end = projected[i];
        glm::dvec2 direction = end.screen - start.screen;
        double length = glm::length(direction);
        if (!(length > 0.0)) continue;
        previousAccumulated = accumulated;
        accumulated += length;
        if (!(accumulated > activationLength)) continue;
        if (previousAccumulated / activationLength < 0.3 &&
            length / activationLength >= 2.0) {
            const double t = activationLength / length;
            start.ecef = start.ecef + (end.ecef - start.ecef) * t;
            start.screen += direction * t;
            start.clipW += (end.clipW - start.clipW) * t;
            direction = end.screen - start.screen;
            length = glm::length(direction);
        }
        const bool reverse = std::abs(direction.y / direction.x) > 1.0
            ? direction.y < 0.0 : direction.x < 0.0;
        if (!readingReverse) readingReverse = reverse;
        double turn = 0.0;
        if (previousStart) {
            const glm::dvec2 incoming = start.screen - previousStart->screen;
            const double divisor = glm::length(incoming) * length;
            if (divisor > 0.0) {
                turn = std::acos(std::clamp(
                    glm::dot(incoming, direction) / divisor,
                    -1.0, 1.0));
            }
        }
        const bool split = turn > kMaximumTurn ||
                           reverse != *readingReverse ||
                           length / segmentUnit < 0.6;
        auto& candidate = candidates.back();
        candidate.push_back(start);
        if (split) {
            candidates.emplace_back();
            readingReverse.reset();
        }
        if (i + 1 == projected.size()) candidates.back().push_back(end);
        previousStart = start;
    }
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [](const auto& value) {
                                        return value.size() < 2;
                                    }), candidates.end());
    std::vector<ProjectedPathSampler> result;
    result.reserve(candidates.size());
    for (auto& candidate : candidates) {
        // labelsUtil.Ed keeps Chinese road names upright by reversing each
        // candidate whose representative direction points left/up.
        const size_t representative = candidate.size() >= 4
            ? candidate.size() * 3 / 4 : candidate.size() - 1;
        const glm::dvec2 representativeDirection =
            candidate[representative].screen - candidate.front().screen;
        const bool reverseCandidate =
            std::abs(representativeDirection.y /
                     representativeDirection.x) > 1.0
                ? representativeDirection.y < 0.0
                : representativeDirection.x < 0.0;
        if (reverseCandidate) std::reverse(candidate.begin(), candidate.end());

        ProjectedPathSampler out;
        out.pathEcef_.reserve(candidate.size());
        out.pathScreenPx_.reserve(candidate.size());
        out.clipW_.reserve(candidate.size());
        out.cumulativeScreenLengthPx_.reserve(candidate.size());
        out.cumulativeScreenLengthPx_.push_back(0.0);
        for (size_t i = 0; i < candidate.size(); ++i) {
            out.pathEcef_.push_back(candidate[i].ecef);
            out.pathScreenPx_.push_back(
                {candidate[i].screen.x, candidate[i].screen.y});
            out.clipW_.push_back(candidate[i].clipW);
            if (i > 0) {
                out.totalScreenLengthPx_ += glm::length(
                    candidate[i].screen - candidate[i - 1].screen);
                out.cumulativeScreenLengthPx_.push_back(
                    out.totalScreenLengthPx_);
            }
        }
        if (out.totalScreenLengthPx_ > 1e-6) result.push_back(std::move(out));
    }
    return result;
}

ProjectedPathSample ProjectedPathSampler::sample(double distancePx) const {
    distancePx = std::clamp(distancePx, 0.0, totalScreenLengthPx_);
    const auto upper = std::upper_bound(cumulativeScreenLengthPx_.begin(),
                                        cumulativeScreenLengthPx_.end(),
                                        distancePx);
    size_t i = static_cast<size_t>(upper - cumulativeScreenLengthPx_.begin());
    i = std::clamp<size_t>(i, 1, pathEcef_.size() - 1);
    const double segment = cumulativeScreenLengthPx_[i] -
                           cumulativeScreenLengthPx_[i - 1];
    const double screenT = segment > 0.0
        ? (distancePx - cumulativeScreenLengthPx_[i - 1]) / segment : 0.0;
    // NDC interpolation is not affine in world-space t because of the
    // perspective divide. Convert screen-linear progress back to the exact
    // world-space parameter using the endpoint clip w values.
    const double w0 = clipW_[i - 1];
    const double w1 = clipW_[i];
    const double denominator = (1.0 - screenT) * w1 + screenT * w0;
    const double t = denominator > 0.0
        ? screenT * w0 / denominator : screenT;
    const Vec3 direction = pathEcef_[i] - pathEcef_[i - 1];
    const Vec3 position = pathEcef_[i - 1] + direction * t;

    // Derive the tangent from the same authoritative projected arc-length
    // contract as the anchor. A centred two-pixel chord remains continuous at
    // encoded vertices; a segment-local direction creates visible kinks and
    // would preserve the retired world-metre tangent behavior as a second
    // path.
    const double halfSpanPx = 2.0;
    const auto positionAt = [&](double d) {
        d = std::clamp(d, 0.0, totalScreenLengthPx_);
        const auto u = std::upper_bound(cumulativeScreenLengthPx_.begin(),
                                        cumulativeScreenLengthPx_.end(), d);
        size_t j = static_cast<size_t>(u - cumulativeScreenLengthPx_.begin());
        j = std::clamp<size_t>(j, 1, pathEcef_.size() - 1);
        const double span = cumulativeScreenLengthPx_[j] -
                            cumulativeScreenLengthPx_[j - 1];
        const double st = span > 0.0
            ? (d - cumulativeScreenLengthPx_[j - 1]) / span : 0.0;
        const double jw0 = clipW_[j - 1];
        const double jw1 = clipW_[j];
        const double denom = (1.0 - st) * jw1 + st * jw0;
        const double wt = denom > 0.0 ? st * jw0 / denom : st;
        return pathEcef_[j - 1] + (pathEcef_[j] - pathEcef_[j - 1]) * wt;
    };
    const Vec3 before = positionAt(distancePx - halfSpanPx);
    const Vec3 after = positionAt(distancePx + halfSpanPx);
    Vec3 tangent = after - before;
    if (tangent.length() <= 1e-6) tangent = direction;
    const auto screenAt = [&](double d) {
        d = std::clamp(d, 0.0, totalScreenLengthPx_);
        const auto u = std::upper_bound(cumulativeScreenLengthPx_.begin(),
                                        cumulativeScreenLengthPx_.end(), d);
        size_t j = static_cast<size_t>(u - cumulativeScreenLengthPx_.begin());
        j = std::clamp<size_t>(j, 1, pathScreenPx_.size() - 1);
        const double span = cumulativeScreenLengthPx_[j] -
                            cumulativeScreenLengthPx_[j - 1];
        const double st = span > 0.0
            ? (d - cumulativeScreenLengthPx_[j - 1]) / span : 0.0;
        return std::array<double, 2>{
            pathScreenPx_[j - 1][0] +
                (pathScreenPx_[j][0] - pathScreenPx_[j - 1][0]) * st,
            pathScreenPx_[j - 1][1] +
                (pathScreenPx_[j][1] - pathScreenPx_[j - 1][1]) * st};
    };
    const auto sb = screenAt(distancePx - halfSpanPx);
    const auto sa = screenAt(distancePx + halfSpanPx);
    const double angle = std::atan2(sa[1] - sb[1], sa[0] - sb[0]);
    return ProjectedPathSample{position, position + tangent, angle};
}

std::vector<OfficialPathGlyphGroup>
ProjectedPathSampler::layoutOfficialGlyphGroups(
    const std::vector<double>& glyphAdvancesPx,
    bool placeAtCandidateStart,
    double providerPixelRatio) const {
    std::vector<OfficialPathGlyphGroup> groups;
    if (glyphAdvancesPx.empty() || !(providerPixelRatio > 0.0) ||
        !std::isfinite(providerPixelRatio)) return groups;
    double textLength = 0.0;
    for (double advance : glyphAdvancesPx) {
        if (!(advance > 0.0) || !std::isfinite(advance)) return {};
        textLength += advance;
    }
    if (textLength > totalScreenLengthPx_) return groups;

    // Fixed LabelLine Yp/zd/Ud contract. Yp walks glyphs forward from the
    // candidate start. After either a completed or rejected group, zd seeks
    // the next group by Wgt + glyphCount * e1. qgt bounds the search at 60.
    constexpr double kGroupBaseSpacingPx = 300.0;
    constexpr double kPerGlyphSpacingPx = 30.0;
    constexpr double kMaximumAdjacentAngle = M_PI / 10.0;
    constexpr uint32_t kMaximumIterations = 60;
    const double groupStep = providerPixelRatio *
        (kGroupBaseSpacingPx +
         glyphAdvancesPx.size() * kPerGlyphSpacingPx);
    double groupStart = placeAtCandidateStart ? 0.0 : groupStep;
    for (uint32_t iteration = 0; iteration < kMaximumIterations;
         ++iteration, groupStart += textLength + groupStep) {
        if (groupStart + textLength > totalScreenLengthPx_) break;
        OfficialPathGlyphGroup group;
        group.groupOrder = iteration;
        group.glyphSamples.reserve(glyphAdvancesPx.size());
        double cursor = groupStart;
        bool valid = true;
        for (double advance : glyphAdvancesPx) {
            cursor += advance;
            ProjectedPathSample glyph = sample(cursor);
            if (!group.glyphSamples.empty() &&
                std::abs(glyph.angleRad -
                         group.glyphSamples.back().angleRad) >=
                    kMaximumAdjacentAngle) {
                valid = false;
                break;
            }
            group.glyphSamples.push_back(glyph);
        }
        if (valid) groups.push_back(std::move(group));
    }
    return groups;
}

}  // namespace earth_engine
