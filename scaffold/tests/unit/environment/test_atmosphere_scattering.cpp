#include <gtest/gtest.h>
#include <cmath>
#include "earth_engine/environment/SkyGradient.h"
#include "earth_engine/environment/SunDirection.h"
#include "earth_engine/environment/TimeController.h"
#include "earth_engine/environment/AtmosphereParameters.h"
#include "earth_engine/environment/SkyColorModel.h"

using namespace earth_engine;

// ============================================================
// 大气散射颜色合理性测试
// 对标 OpenGlobus SimpleSkyBackground 行为
// ============================================================

// 2024-06-21 12:00 UTC (June solstice, sun high in northern hemisphere)
static double juneSolsticeJD() {
    return unixToJulian(1718967600.0);
}

// 2024-12-21 12:00 UTC (December solstice, sun in southern hemisphere)
static double decemberSolsticeJD() {
    return unixToJulian(1734865200.0);
}

// 2024-03-20 06:00 UTC (equinox, sunrise at prime meridian)
static double equinoxMorningJD() {
    return unixToJulian(1710914400.0);
}

// Midnight at prime meridian
static double midnightJD() {
    return unixToJulian(1718942400.0);  // 2024-06-21 00:00 UTC
}

class AtmosphereScatteringTest : public ::testing::Test {
protected:
    SkyGradient gradient_;
    Vec3 localUp_ = Vec3::unitX();  // Greenwich equator test point.
};

TEST_F(AtmosphereScatteringTest, ZenithBlueAtNoon) {
    // 夏至中午：天顶应该是蓝色
    Vec3 sunDir = SunDirection::compute(juneSolsticeJD());
    gradient_.update(sunDir, localUp_, 0.0);  // sea level

    auto& zenith = gradient_.zenithColor();
    // 蓝色分量应大于红色分量（Rayleigh 散射）
    EXPECT_GT(zenith[2], zenith[0]);  // B > R
    EXPECT_GT(zenith[2], 0.15f);      // 显著的蓝色

    // RGBA
    EXPECT_NEAR(zenith[3], 1.0f, 1e-6f);
}

TEST_F(AtmosphereScatteringTest, HorizonBrighterThanZenith) {
    // 地平线应该比天顶亮（更长的大气路径 → 更多散射光）
    Vec3 sunDir = SunDirection::compute(juneSolsticeJD());
    gradient_.update(sunDir, localUp_, 0.0);

    auto& zenith = gradient_.zenithColor();
    auto& horizon = gradient_.horizonColor();

    // 地平线亮度 > 天顶亮度（至少一个通道）
    float zenithLum = zenith[0] + zenith[1] + zenith[2];
    float horizonLum = horizon[0] + horizon[1] + horizon[2];
    EXPECT_GT(horizonLum, zenithLum);
}

TEST_F(AtmosphereScatteringTest, NightSkyIsDark) {
    // 午夜：天空应该非常暗
    Vec3 sunDir = SunDirection::compute(midnightJD());
    gradient_.update(sunDir, localUp_, 0.0);

    auto& zenith = gradient_.zenithColor();
    auto& ambient = gradient_.ambientColor();

    // 天顶很暗
    EXPECT_LT(zenith[0] + zenith[1] + zenith[2], 0.15f);

    // 环境光很小但非零（星光）
    EXPECT_GT(ambient[0] + ambient[1] + ambient[2], 0.001f);
    EXPECT_LT(ambient[0] + ambient[1] + ambient[2], 0.1f);
}

TEST_F(AtmosphereScatteringTest, SunElevationMatchesSunDirection) {
    Vec3 sunDir = SunDirection::compute(juneSolsticeJD());
    gradient_.update(sunDir, localUp_, 0.0);

    double elevation = gradient_.sunElevation();
    // 太阳方向相对当前位置椭球法线的夹角应与仰角一致
    double expectedElev = std::asin(sunDir.dot(localUp_));
    EXPECT_NEAR(elevation, expectedElev, 1e-6);
}

TEST_F(AtmosphereScatteringTest, CameraAltitudeReducesAtmosphere) {
    Vec3 sunDir = SunDirection::compute(juneSolsticeJD());

    // 地面:spaceFactor=0,天空亮(与 GPU computeSkyColor 同源)
    gradient_.update(sunDir, localUp_, 0.0);
    auto groundHorizon = gradient_.horizonColor();
    float groundBrightness = groundHorizon[0] + groundHorizon[1] + groundHorizon[2];

    // 深空（900km，spaceFactor=1）:computeSkyColor 压向太空黑,天顶应明显变暗
    gradient_.update(sunDir, localUp_, 900000.0);
    auto spaceHorizon = gradient_.horizonColor();
    float spaceBrightness = spaceHorizon[0] + spaceHorizon[1] + spaceHorizon[2];

    // 太空色 = kSkySpace(压黑项主导)
    EXPECT_LT(spaceBrightness, groundBrightness * 0.9f);
}

// L-P1:clear/ambient 必须与 GPU 天空同一色模型 —— SkyGradient 白天输出应
// 精确等于 computeSkyColorCpu 采样(地平线方向朝太阳),不再存在两套可漂移的
// 色板(正午 horizon 曾差 7 倍)。
TEST_F(AtmosphereScatteringTest, DaytimeMatchesComputeSkyColorSingleSource) {
    Vec3 sunDir = SunDirection::compute(juneSolsticeJD());
    gradient_.update(sunDir, localUp_, 0.0);

    auto& horizon = gradient_.horizonColor();
    auto& zenith = gradient_.zenithColor();

    // 参考:computeSkyColorCpu 同一输入
    const Vec3 up = localUp_.normalized();
    const Vec3 sun = sunDir.normalized();
    const std::array<double, 3> upArr = {up.x(), up.y(), up.z()};
    const std::array<double, 3> sunArr = {sun.x(), sun.y(), sun.z()};
    const double sunUpDot = sunArr[0] * upArr[0] + sunArr[1] * upArr[1] +
                            sunArr[2] * upArr[2];
    std::array<double, 3> horizDir = {
        sunArr[0] - upArr[0] * sunUpDot,
        sunArr[1] - upArr[1] * sunUpDot,
        sunArr[2] - upArr[2] * sunUpDot};
    const double hl = std::sqrt(horizDir[0] * horizDir[0] +
                                horizDir[1] * horizDir[1] +
                                horizDir[2] * horizDir[2]);
    horizDir[0] /= hl;
    horizDir[1] /= hl;
    horizDir[2] /= hl;

    const std::array<double, 3> refHorizon =
        computeSkyColorCpu(horizDir, upArr, sunArr, 0.0);
    const std::array<double, 3> refZenith =
        computeSkyColorCpu(upArr, upArr, sunArr, 0.0);

    EXPECT_NEAR(horizon[0], refHorizon[0], 1e-6f);
    EXPECT_NEAR(horizon[1], refHorizon[1], 1e-6f);
    EXPECT_NEAR(horizon[2], refHorizon[2], 1e-6f);
    EXPECT_NEAR(zenith[0], refZenith[0], 1e-6f);
    EXPECT_NEAR(zenith[1], refZenith[1], 1e-6f);
    EXPECT_NEAR(zenith[2], refZenith[2], 1e-6f);
}

TEST_F(AtmosphereScatteringTest, AllColorsInValidRange) {
    // 测试多种条件
    struct TestCase {
        double jd;
        double altitude;
    };
    TestCase cases[] = {
        {juneSolsticeJD(), 0.0},
        {juneSolsticeJD(), 10000.0},
        {juneSolsticeJD(), 100000.0},
        {decemberSolsticeJD(), 0.0},
        {equinoxMorningJD(), 0.0},
        {midnightJD(), 0.0},
    };

    for (const auto& tc : cases) {
        Vec3 sunDir = SunDirection::compute(tc.jd);
        gradient_.update(sunDir, localUp_, tc.altitude);

        auto& z = gradient_.zenithColor();
        auto& h = gradient_.horizonColor();
        auto& a = gradient_.ambientColor();

        // 所有颜色分量在 [0, 1]
        for (int c = 0; c < 3; ++c) {
            EXPECT_GE(z[c], 0.0f) << "zenith[" << c << "] < 0";
            EXPECT_LE(z[c], 1.0f) << "zenith[" << c << "] > 1";
            EXPECT_GE(h[c], 0.0f) << "horizon[" << c << "] < 0";
            EXPECT_LE(h[c], 1.0f) << "horizon[" << c << "] > 1";
            EXPECT_GE(a[c], 0.0f) << "ambient[" << c << "] < 0";
            EXPECT_LE(a[c], 1.0f) << "ambient[" << c << "] > 1";
        }
        EXPECT_FLOAT_EQ(z[3], 1.0f);
        EXPECT_FLOAT_EQ(h[3], 1.0f);
    }
}

TEST_F(AtmosphereScatteringTest, MarsParametersDifferFromEarth) {
    AtmosphereParameters earthParams = earthAtmosphereDefaults();
    AtmosphereParameters marsParams = marsAtmosphereDefaults();

    // 火星参数应该有区别
    EXPECT_NE(marsParams.atmosHeight, earthParams.atmosHeight)
        << "Mars atmosphere height should differ from Earth";
    EXPECT_NE(marsParams.bottomRadius, earthParams.bottomRadius)
        << "Mars radius should differ from Earth";
    EXPECT_GT(marsParams.rayleighSeaLevelScattering, earthParams.rayleighSeaLevelScattering)
        << "Mars has more dust (higher Mie scattering)";
}

TEST_F(AtmosphereScatteringTest, TopRadiusCalculatedCorrectly) {
    AtmosphereParameters params = earthAtmosphereDefaults();
    EXPECT_NEAR(params.topRadius(),
                params.bottomRadius + params.atmosHeight,
                1e-6);
}

TEST_F(AtmosphereScatteringTest, ValidateDefaultParameters) {
    AtmosphereParameters params;
    params.validate();

    EXPECT_GT(params.atmosHeight, 0.0);
    EXPECT_GT(params.rayleighScaleHeight, 0.0);
    EXPECT_GT(params.mieScaleHeight, 0.0);
    EXPECT_GT(params.bottomRadius, 0.0);
}
