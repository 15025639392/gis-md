#include <gtest/gtest.h>
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/layers/ProjectedPathSampler.h"
#include "earth_engine/scene/Camera.h"
#include <glm/glm.hpp>
#include <cmath>

using namespace earth_engine;

namespace {
glm::dvec2 project(const Vec3& p, const Mat4& vp, double w, double h) {
    const glm::dvec4 c = vp.raw() * glm::dvec4(p.x(), p.y(), p.z(), 1.0);
    return {(c.x / c.w * 0.5 + 0.5) * w,
            (c.y / c.w * 0.5 + 0.5) * h};
}
}

TEST(ProjectedPathSamplerTest, PitchedPerspectiveExposesGroundArcError) {
    const Ellipsoid ellipsoid = Ellipsoid::WGS84();
    constexpr double w = 1080.0, h = 1920.0;
    const Cartographic c(106.55 * M_PI / 180.0, 29.56 * M_PI / 180.0);
    const Vec3 target = ellipsoid.cartographicToCartesian(c);
    const Vec3 up = target.normalized();
    const Vec3 east(-std::sin(c.longitude()), std::cos(c.longitude()), 0.0);
    const Vec3 north = up.cross(east).normalized();
    Camera camera;
    camera.lookAt(target + up * 3500.0 - north * 5000.0, target, up);
    const Mat4 vp = camera.viewProjectionMatrix(w, h);
    const std::vector<Vec3> path{
        target - north * 2200.0 - east * 500.0,
        target - north * 400.0,
        target + north * 2600.0 + east * 700.0};
    const auto candidates = ProjectedPathSampler::createCandidates(path, vp, w, h);
    ASSERT_FALSE(candidates.empty());
    const auto& sampler = candidates.front();
    const Vec3 screenMid = sampler.sample(
        sampler.totalScreenLengthPx() * 0.5).positionEcef;
    const double a = (path[1] - path[0]).length();
    const double b = (path[2] - path[1]).length();
    const double half = (a + b) * 0.5;
    const Vec3 groundMid = half <= a
        ? path[0] + (path[1] - path[0]) * (half / a)
        : path[1] + (path[2] - path[1]) * ((half - a) / b);
    EXPECT_GT(glm::length(project(screenMid, vp, w, h) -
                          project(groundMid, vp, w, h)), 8.0);
    // The official Zp candidate contract may trim the leading part of a long
    // first segment before screen-linear sampling. Its selected candidate is
    // therefore intentionally different from the complete source path.
    EXPECT_GT(sampler.totalScreenLengthPx(), 100.0);
    EXPECT_TRUE(std::isfinite(project(screenMid, vp, w, h).x));
}

TEST(ProjectedPathSamplerTest, RejectsBehindCameraPath) {
    Camera camera;
    camera.lookAt(Vec3(10.0, 0.0, 0.0), Vec3::zero(), Vec3(0, 0, 1));
    EXPECT_TRUE(ProjectedPathSampler::createCandidates(
        {Vec3(20, 0, 0), Vec3(21, 0, 0)},
        camera.viewProjectionMatrix(800, 600), 800, 600).empty());
}

TEST(ProjectedPathSamplerTest, OfficialCandidateDoesNotBridgeRightAngleCorner) {
    Camera camera;
    camera.lookAt(Vec3(0, 0, 10), Vec3::zero(), Vec3(0, 1, 0));
    const Mat4 vp = camera.viewProjectionMatrix(800, 600);
    const std::vector<Vec3> path{
        Vec3(-2, 0, 0), Vec3(-1, 0, 0), Vec3::zero(),
        Vec3(0, 1, 0), Vec3(0, 2, 0)};
    const auto candidates = ProjectedPathSampler::createCandidates(
        path, vp, 800, 600);
    ASSERT_FALSE(candidates.empty());
    EXPECT_GE(candidates.size(), 2u);
    const ProjectedPathSample corner = candidates.front().sample(
        candidates.front().totalScreenLengthPx() * 0.5);
    const Vec3 tangent = corner.tangentEcef - corner.positionEcef;
    EXPECT_GT(tangent.length(), 1e-6);
    EXPECT_TRUE(std::abs(tangent.x()) < 1e-6 ||
                std::abs(tangent.y()) < 1e-6);
}

TEST(ProjectedPathSamplerTest, OfficialHairpinCandidateReadsUpright) {
    Camera camera;
    camera.lookAt(Vec3(0, 0, 10), Vec3::zero(), Vec3(0, 1, 0));
    const Mat4 vp = camera.viewProjectionMatrix(800, 600);
    const std::vector<Vec3> path{
        Vec3(-1, 0, 0), Vec3::zero(), Vec3(-1, 0, 0)};
    const auto candidates = ProjectedPathSampler::createCandidates(
        path, vp, 800, 600);
    ASSERT_FALSE(candidates.empty());
    const ProjectedPathSample corner = candidates.front().sample(
        candidates.front().totalScreenLengthPx() * 0.5);
    const Vec3 tangent = corner.tangentEcef - corner.positionEcef;
    EXPECT_GT(tangent.length(), 1e-6);
    EXPECT_GT(tangent.x(), 0.0);
    EXPECT_NEAR(tangent.y(), 0.0, 1e-12);
}

TEST(ProjectedPathSamplerTest, OfficialYpProducesRepeatedGroupsAtFixedStep) {
    const auto candidates = ProjectedPathSampler::createCandidates(
        {Vec3(-1, 0, 0), Vec3(1, 0, 0)}, Mat4::identity(), 800, 600,
        10.0, 13.0);
    ASSERT_EQ(candidates.size(), 1u);
    const auto groups = candidates.front().layoutOfficialGlyphGroups(
        {13.0, 13.0});
    ASSERT_EQ(groups.size(), 2u);
    ASSERT_EQ(groups[0].glyphSamples.size(), 2u);
    ASSERT_EQ(groups[1].glyphSamples.size(), 2u);
    EXPECT_EQ(groups[0].groupOrder, 0u);
    EXPECT_EQ(groups[1].groupOrder, 1u);
    const auto first = project(groups[0].glyphSamples[0].positionEcef,
                               Mat4::identity(), 800, 600);
    const auto repeated = project(groups[1].glyphSamples[0].positionEcef,
                                  Mat4::identity(), 800, 600);
    // After the first group is consumed, zd skips Wgt + glyphCount * e1 =
    // 300 + 2 * 30 framebuffer pixels before the next group starts.
    EXPECT_NEAR(repeated.x - first.x, 13.0 * 2.0 + 360.0, 1e-9);
}

TEST(ProjectedPathSamplerTest, OfficialLaterCandidateSkipsInitialGroupSlot) {
    const auto candidates = ProjectedPathSampler::createCandidates(
        {Vec3(-1, 0, 0), Vec3(1, 0, 0)}, Mat4::identity(), 800, 600,
        10.0, 13.0);
    ASSERT_EQ(candidates.size(), 1u);
    const auto first = candidates.front().layoutOfficialGlyphGroups(
        {13.0, 13.0}, true);
    const auto later = candidates.front().layoutOfficialGlyphGroups(
        {13.0, 13.0}, false);
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(later.empty());
    const auto a = project(first.front().glyphSamples.front().positionEcef,
                           Mat4::identity(), 800, 600);
    const auto b = project(later.front().glyphSamples.front().positionEcef,
                           Mat4::identity(), 800, 600);
    EXPECT_NEAR(b.x - a.x, 360.0, 1e-9);
}

TEST(ProjectedPathSamplerTest, OfficialGroupSpacingAppliesDprExactlyOnce) {
    const auto dpr1Candidates = ProjectedPathSampler::createCandidates(
        {Vec3(-1, 0, 0), Vec3(1, 0, 0)}, Mat4::identity(), 800, 600,
        10.0, 13.0);
    const auto dpr2Candidates = ProjectedPathSampler::createCandidates(
        {Vec3(-1, 0, 0), Vec3(1, 0, 0)}, Mat4::identity(), 1600, 1200,
        20.0, 13.0, 2.0);
    ASSERT_EQ(dpr1Candidates.size(), 1u);
    ASSERT_EQ(dpr2Candidates.size(), 1u);
    const auto dpr1 = dpr1Candidates.front().layoutOfficialGlyphGroups(
        {13.0, 13.0}, false, 1.0);
    const auto dpr2 = dpr2Candidates.front().layoutOfficialGlyphGroups(
        {26.0, 26.0}, false, 2.0);
    ASSERT_FALSE(dpr1.empty());
    ASSERT_FALSE(dpr2.empty());
    const auto a = project(dpr1.front().glyphSamples.front().positionEcef,
                           Mat4::identity(), 800, 600);
    const auto b = project(dpr2.front().glyphSamples.front().positionEcef,
                           Mat4::identity(), 1600, 1200);
    EXPECT_NEAR(b.x, a.x * 2.0, 1e-9);
}

TEST(ProjectedPathSamplerTest, OfficialYpSearchIsCappedAtSixtyIterations) {
    const auto candidates = ProjectedPathSampler::createCandidates(
        {Vec3(0, 0, 0), Vec3(500, 0, 0)}, Mat4::identity(), 100, 100,
        10.0, 13.0);
    ASSERT_EQ(candidates.size(), 1u);
    const auto groups = candidates.front().layoutOfficialGlyphGroups({13.0});
    ASSERT_EQ(groups.size(), 60u);
    EXPECT_EQ(groups.front().groupOrder, 0u);
    EXPECT_EQ(groups.back().groupOrder, 59u);
}

TEST(ProjectedPathSamplerTest, OfficialYpRejectsTextLongerThanCandidate) {
    const auto candidates = ProjectedPathSampler::createCandidates(
        {Vec3(-0.2, 0, 0), Vec3(0.2, 0, 0)}, Mat4::identity(), 800, 600,
        10.0, 13.0);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_TRUE(candidates.front().layoutOfficialGlyphGroups(
        {100.0, 100.0}).empty());
}
