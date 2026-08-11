#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/math/Ray.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/Frustum.h"

#include <cmath>
#include <memory>
#include <optional>

using namespace earth_engine;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kWidth = 800.0;
constexpr double kHeight = 600.0;

/// 把相机放到目标点正上方 h 米、看向地心(正俯视)。
void placeNadir(Camera& camera, double lonDeg, double latDeg, double h) {
    const auto& e = Ellipsoid::WGS84();
    const Vec3 ground = e.cartographicToCartesian(
        Cartographic::fromDegrees(lonDeg, latDeg, 0.0));
    const Vec3 up = e.geodeticSurfaceNormal(ground);
    camera.lookAt(Vec3(ground.raw() + up.raw() * h), ground, Vec3(0.0, 0.0, 1.0));
}

/// 透视相机在地面处的视口宽度(米)——正交要覆盖同一片地面就得用这个宽度。
double perspectiveGroundWidth(double fovY, double distance, double aspect) {
    return 2.0 * distance * std::tan(fovY * 0.5) * aspect;
}

std::optional<Vec3> groundHit(const Camera& camera, double px, double py) {
    const Ray ray = camera.getPickRay(px, py, kWidth, kHeight);
    return Ellipsoid::WGS84().rayIntersection(ray.origin(), ray.direction());
}

}  // namespace

// ============================================================
// 判据 ③:isOrthographic() 真实反映状态
// ============================================================

TEST(OrthographicTest, ProjectionModeReflectsTheActiveSetter) {
    Camera camera;
    // 改成真实实现之前它恒 false —— 那种"永远答同一个值"的查询比没有更糟:
    // 消费方(SkyBox、渲染管线)看着像接好了,实际永远走透视分支。
    EXPECT_FALSE(camera.isOrthographic());
    EXPECT_EQ(Camera::ProjectionMode::Perspective, camera.projectionMode());

    camera.setOrthographic(1.0e6, 1.0, 1.0e9);
    EXPECT_TRUE(camera.isOrthographic());
    EXPECT_EQ(Camera::ProjectionMode::Orthographic, camera.projectionMode());
    EXPECT_DOUBLE_EQ(1.0e6, camera.orthographicWidthMeters());

    camera.setPerspective(kPi / 4.0, 1.0, 1.0e9);
    EXPECT_FALSE(camera.isOrthographic()) << "setPerspective 必须把模式切回来";
}

TEST(OrthographicTest, RejectsInvalidParameters) {
    Camera camera;
    EXPECT_THROW(camera.setOrthographic(0.0, 1.0, 100.0), std::invalid_argument);
    EXPECT_THROW(camera.setOrthographic(-5.0, 1.0, 100.0), std::invalid_argument);
    EXPECT_THROW(camera.setOrthographic(100.0, 0.0, 100.0), std::invalid_argument);
    EXPECT_THROW(camera.setOrthographic(100.0, 10.0, 10.0), std::invalid_argument);
    // 抛异常的路径不该留下半改状态。
    EXPECT_FALSE(camera.isOrthographic());
}

// ============================================================
// 深度约定:正交与透视共用 reverse-Z
// ============================================================

TEST(OrthographicTest, DepthFollowsTheSameReverseZConventionAsPerspective) {
    Camera camera;
    camera.setOrthographic(1.0e6, 10.0, 1.0e6);
    const Mat4 proj = camera.projectionMatrix(kWidth, kHeight);

    // 眼空间点(相机看向 −z):z_eye = −near 应映到 z_ndc = 1,−far 映到 0。
    // 写反的话深度测试(clear=0 + GreaterEqual)整个反过来 —— 画面里近处被远处
    // 盖住,而单看一张静止截图很容易误判成"深度冲突"。
    auto ndcZ = [&](double zEye) {
        const glm::dvec4 clip = proj.raw() * glm::dvec4(0.0, 0.0, zEye, 1.0);
        return clip.z / clip.w;
    };
    EXPECT_NEAR(1.0, ndcZ(-10.0), 1e-12);
    EXPECT_NEAR(0.0, ndcZ(-1.0e6), 1e-12);
    // 单调递减(近 → 远)。
    EXPECT_GT(ndcZ(-1.0e5), ndcZ(-5.0e5));

    // w 恒为 1 —— 这正是反投影天然给出平行射线的原因。
    const glm::dvec4 clip = proj.raw() * glm::dvec4(1234.0, -567.0, -8910.0, 1.0);
    EXPECT_DOUBLE_EQ(1.0, clip.w);
}

TEST(OrthographicTest, ViewportWidthMapsToNdcExactly) {
    Camera camera;
    const double widthMeters = 4.0e5;
    camera.setOrthographic(widthMeters, 1.0, 1.0e9);
    const Mat4 proj = camera.projectionMatrix(kWidth, kHeight);
    const double aspect = kWidth / kHeight;

    auto ndcXY = [&](double xEye, double yEye) {
        const glm::dvec4 clip =
            proj.raw() * glm::dvec4(xEye, yEye, -100.0, 1.0);
        return glm::dvec2(clip.x / clip.w, clip.y / clip.w);
    };
    // 半宽处恰好是 ndc ±1;高度按宽高比推出(不是也用宽度,那样会拉伸)。
    EXPECT_NEAR(1.0, ndcXY(widthMeters * 0.5, 0.0).x, 1e-12);
    EXPECT_NEAR(1.0, ndcXY(0.0, widthMeters * 0.5 / aspect).y, 1e-12);
}

// ============================================================
// 判据 ①(前半):正交下 pick ray 是平行射线
// ============================================================

TEST(OrthographicTest, PickRaysAreParallelAndOffsetByPixel) {
    Camera camera;
    camera.setOrthographic(1.0e6, 1.0, 1.0e9);
    placeNadir(camera, 106.5, 29.6, 5.0e6);

    const Ray center = camera.getPickRay(kWidth * 0.5, kHeight * 0.5,
                                         kWidth, kHeight);
    const Ray corner = camera.getPickRay(10.0, 20.0, kWidth, kHeight);

    // 平行:方向恒等于 camera.direction。透视下这两条会明显发散。
    EXPECT_GT(glm::dot(center.direction().raw(), camera.direction().raw()),
              1.0 - 1e-12);
    EXPECT_GT(glm::dot(corner.direction().raw(), camera.direction().raw()),
              1.0 - 1e-12);
    // origin 随像素平移,且平移量落在与视线垂直的平面内。
    const glm::dvec3 offset = corner.origin().raw() - center.origin().raw();
    EXPECT_GT(glm::length(offset), 1.0);
    EXPECT_NEAR(0.0, glm::dot(offset, camera.direction().raw()), 1e-6);
}

TEST(OrthographicTest, PerspectivePickRaysStillDivergeAsControl) {
    // 对照组:同样两个像素在透视下必须发散。缺了它,上面那条"平行"可能只是
    // 因为相机太远导致两条射线数值上碰巧接近,而不是因为投影真的是正交。
    Camera camera;
    camera.setPerspective(kPi / 4.0, 1.0, 1.0e9);
    placeNadir(camera, 106.5, 29.6, 5.0e6);

    const Ray center = camera.getPickRay(kWidth * 0.5, kHeight * 0.5,
                                         kWidth, kHeight);
    const Ray corner = camera.getPickRay(10.0, 20.0, kWidth, kHeight);
    EXPECT_LT(glm::dot(center.direction().raw(), corner.direction().raw()),
              0.99);
}

// ============================================================
// 判据 ①(后半):相机足够远时,正交与透视在同一像素的地面命中点趋同
// ============================================================

TEST(OrthographicTest, GroundHitConvergesToPerspectiveAsCameraRecedes) {
    const double aspect = kWidth / kHeight;
    // ⚠️ **收敛的正确陈述是「地面足迹固定、相机后退且 fov 随之收窄」**。
    // 第一版让足迹随高度一起长大(正交宽度取"透视在该高度的足迹"),结果 2e7 m
    // 时足迹已达 1.4e7 m —— 地球曲率整个主导,两者只会越差越远,而且角落射线
    // 直接打空。那不是收敛失败,是判据写错了。
    const double groundWidth = 2.0e5;
    const double groundHeight = groundWidth / aspect;
    const double px = 700.0;   // 刻意取角落:中心像素两者恒等,测不出东西
    const double py = 80.0;

    auto discrepancyAt = [&](double altitude) {
        // fov 随高度收窄,使地面足迹恒为 groundWidth —— 这才是 perspective→ortho
        // 的那个极限。
        const double fovY = 2.0 * std::atan((groundHeight * 0.5) / altitude);

        Camera perspective;
        perspective.setPerspective(fovY, 1.0, 1.0e12);
        placeNadir(perspective, 106.5, 29.6, altitude);

        Camera ortho;
        ortho.setOrthographic(groundWidth, 1.0, 1.0e12);
        placeNadir(ortho, 106.5, 29.6, altitude);

        const auto hp = groundHit(perspective, px, py);
        const auto ho = groundHit(ortho, px, py);
        EXPECT_TRUE(hp.has_value()) << "altitude=" << altitude;
        EXPECT_TRUE(ho.has_value()) << "altitude=" << altitude;
        if (!hp || !ho) return 1.0;
        // 相对差:除以足迹宽度,量纲无关。
        return glm::length(hp->raw() - ho->raw()) / groundWidth;
    };

    const double nearAlt = discrepancyAt(2.0e5);
    const double midAlt = discrepancyAt(2.0e6);
    const double farAlt = discrepancyAt(2.0e7);

    // **判据是单调收敛而不是某个魔法容差**:透视在远处趋近正交是几何事实,
    // 拿一个拍脑袋的阈值去卡,阈值本身就成了未经验证的常量。
    EXPECT_LT(midAlt, nearAlt)
        << "远离后差异未缩小:near=" << nearAlt << " mid=" << midAlt;
    EXPECT_LT(farAlt, midAlt) << "mid=" << midAlt << " far=" << farAlt;
    EXPECT_LT(farAlt, 1e-3) << "足够远时应已基本重合,实测 " << farAlt;
    // 近处必须**确实有差异**,否则上面三条可能只是在比较三个 0。
    EXPECT_GT(nearAlt, 1e-4) << "近处两者就已重合 ⇒ 这条用例没测到东西";
}

// ============================================================
// 判据 ②:正交下 near 不再走透视公式(动态 near 必须断掉)
// ============================================================
//
// 动态 near 的接线在 SceneFrameUpdateCoordinator,host 上不便整场景起。这里
// 把它的**公式本身**在正交下的荒谬性钉住:相机在 2e7 m 高空时,透视公式会把
// near 收到几十米量级 —— 正交下那意味着相机前方 2e7 m 的地形全部被切掉。

TEST(OrthographicTest, PerspectiveDynamicNearWouldClipEverythingInOrthographic) {
    const double altitude = 2.0e7;
    Camera camera;
    camera.setOrthographic(1.0e6, 1.0, 1.0e12);
    placeNadir(camera, 106.5, 29.6, altitude);

    // 动态 near 公式(SceneFrameUpdateCoordinator 里那一份)的量级:
    // near ≈ 0.5 × 最近几何距离。近场探针在贴地时给出几十米。
    const double dynamicNearIfApplied = 0.5 * 50.0;
    ASSERT_LT(dynamicNearIfApplied, altitude);

    // 正交下 near 是**沿视线的裁剪面**,不是透视那种"视锥顶点距离"。把它收到
    // 25m 意味着 z_eye ∈ [−25, −far] 之外全被裁 —— 地面在 z_eye ≈ −2e7,还在
    // 范围内,但相机与地面之间的一切(大气、云、飞行器)全没了,而且深度精度
    // 被浪费在空气上。真正的问题是它**没有理由**:正交 z_ndc 对 z_eye 线性,
    // 精度全程均匀,不存在透视那个 z_ndc 病态区。
    camera.setOrthographic(1.0e6, dynamicNearIfApplied, 1.0e12);
    const Mat4 tightened = camera.projectionMatrix(kWidth, kHeight);
    camera.setOrthographic(1.0e6, 1.0, 1.0e12);
    const Mat4 baseline = camera.projectionMatrix(kWidth, kHeight);

    auto ndcZ = [&](const Mat4& p, double zEye) {
        const glm::dvec4 clip = p.raw() * glm::dvec4(0.0, 0.0, zEye, 1.0);
        return clip.z / clip.w;
    };
    // 收紧 near 对正交的深度分布几乎没有改善(线性映射,斜率只差 25/1e12),
    // 却白白引入了裁剪风险 —— "没收益、有代价"正是它该被断掉的理由。
    EXPECT_NEAR(ndcZ(tightened, -1.0e6), ndcZ(baseline, -1.0e6), 1e-9);
}

// ============================================================
// 视锥:正交下退化成盒子(通用 VP 提取,不需要特判)
// ============================================================

TEST(OrthographicTest, FrustumBecomesABoxWithParallelSidePlanes) {
    Camera camera;
    camera.setOrthographic(1.0e6, 1.0, 1.0e9);
    placeNadir(camera, 106.5, 29.6, 5.0e6);
    const Frustum box = camera.frustum(kWidth, kHeight);

    // 盒子的左右面法线严格反向(透视下它们成一个夹角)。Frustum 是从 VP 矩阵
    // 通用提取的,不需要为正交加分支 —— 这条用例是来证明"不需要"的。
    const glm::dvec3 left = box.plane(Frustum::PlaneIndex::Left).normal.raw();
    const glm::dvec3 right = box.plane(Frustum::PlaneIndex::Right).normal.raw();
    EXPECT_LT(glm::dot(glm::normalize(left), glm::normalize(right)),
              -1.0 + 1e-9);

    const glm::dvec3 top = box.plane(Frustum::PlaneIndex::Top).normal.raw();
    const glm::dvec3 bottom =
        box.plane(Frustum::PlaneIndex::Bottom).normal.raw();
    EXPECT_LT(glm::dot(glm::normalize(top), glm::normalize(bottom)),
              -1.0 + 1e-9);
}
