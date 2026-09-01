#pragma once

#include "../core/math/Mat4.h"
#include "../core/math/Vec3.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace earth_engine {

struct ProjectedPathSample {
    Vec3 positionEcef = Vec3::zero();
    Vec3 tangentEcef = Vec3::zero();
    double angleRad = 0.0;
};

struct OfficialPathGlyphGroup {
    std::vector<ProjectedPathSample> glyphSamples;
    uint32_t groupOrder = 0;
};

class ProjectedPathSampler {
public:
    static std::vector<ProjectedPathSampler> createCandidates(
        const std::vector<Vec3>& pathEcef, const Mat4& viewProjection,
        double viewportWidthPx, double viewportHeightPx,
        double officialFontSizePx = 12.0, double displayZoom = 13.0,
        double providerPixelRatio = 1.0);
    double totalScreenLengthPx() const { return totalScreenLengthPx_; }
    ProjectedPathSample sample(double distancePx) const;
    std::vector<OfficialPathGlyphGroup> layoutOfficialGlyphGroups(
        const std::vector<double>& glyphAdvancesPx,
        bool placeAtCandidateStart = true,
        double providerPixelRatio = 1.0) const;

private:
    std::vector<Vec3> pathEcef_;
    std::vector<std::array<double, 2>> pathScreenPx_;
    std::vector<double> cumulativeScreenLengthPx_;
    std::vector<double> clipW_;
    double totalScreenLengthPx_ = 0.0;
};

}  // namespace earth_engine
