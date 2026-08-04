#include <gtest/gtest.h>

#include "earth_engine/renderer/VectorPageDrawer.h"

#include <array>
#include <cmath>

using namespace earth_engine;

namespace {

/// 用矩阵变换一个瓦片本地归一化点 → NDC(列主序,z=0,w=1)。
std::array<float, 2> apply(const float m[16], double x, double y) {
    return {static_cast<float>(m[0] * x + m[4] * y + m[12]),
            static_cast<float>(m[1] * x + m[5] * y + m[13])};
}

}  // namespace

// depth=0(页与源同级):整块瓦片铺满 NDC。
TEST(VectorPageDrawerOrtho, FullTileMapsToFullNdc) {
    float m[16];
    VectorPageDrawer::makePageOrtho(0.0, 0.0, 1.0, /*flipY=*/false, m);
    const auto nw = apply(m, 0.0, 0.0);
    const auto se = apply(m, 1.0, 1.0);
    EXPECT_NEAR(nw[0], -1.0f, 1e-5f);
    EXPECT_NEAR(nw[1], -1.0f, 1e-5f);
    EXPECT_NEAR(se[0], 1.0f, 1e-5f);
    EXPECT_NEAR(se[1], 1.0f, 1e-5f);
}

// **子矩形放大 = C-2 干掉 8 倍糊的机制**:页比源深 3 级时只取源瓦片 1/8 的
// 子矩形,几何被投影放大而不是位图被采样放大。
TEST(VectorPageDrawerOrtho, SubRectFillsPageAtDepth) {
    const double span = 1.0 / 8.0;  // depth=3
    const double originX = 5 * span;
    const double originY = 2 * span;
    float m[16];
    VectorPageDrawer::makePageOrtho(originX, originY, span, false, m);

    const auto tl = apply(m, originX, originY);
    const auto br = apply(m, originX + span, originY + span);
    EXPECT_NEAR(tl[0], -1.0f, 1e-4f);
    EXPECT_NEAR(tl[1], -1.0f, 1e-4f);
    EXPECT_NEAR(br[0], 1.0f, 1e-4f);
    EXPECT_NEAR(br[1], 1.0f, 1e-4f);
    // 子矩形之外的几何落在 NDC 之外 → 被裁掉(不会串到邻页)。
    const auto outside = apply(m, originX - span, originY);
    EXPECT_LT(outside[0], -1.0f);
}

// **后端 y 约定只在这个矩阵里分叉**。GL 的 FBO 行 0 在 NDC -1 侧、Metal 的
// render target 行 0 在 +1 侧;瓦片 y=0 恒为北,页行 0 恒为北。
// 搞反的现象是「路网上下镜像」——在对称路网上极难一眼看出。
TEST(VectorPageDrawerOrtho, FlipYInvertsOnlyTheYAxis) {
    float gl[16];
    float metal[16];
    VectorPageDrawer::makePageOrtho(0.0, 0.0, 1.0, /*flipY=*/false, gl);
    VectorPageDrawer::makePageOrtho(0.0, 0.0, 1.0, /*flipY=*/true, metal);

    const auto north_gl = apply(gl, 0.5, 0.0);
    const auto north_mtl = apply(metal, 0.5, 0.0);
    EXPECT_NEAR(north_gl[0], north_mtl[0], 1e-5f) << "x 不受影响";
    EXPECT_NEAR(north_gl[1], -1.0f, 1e-5f);
    EXPECT_NEAR(north_mtl[1], 1.0f, 1e-5f);
}

// span=0 不该产出 NaN/Inf(退化输入走安全默认)。
TEST(VectorPageDrawerOrtho, ZeroSpanDoesNotProduceNonFinite) {
    float m[16];
    VectorPageDrawer::makePageOrtho(0.0, 0.0, 0.0, false, m);
    for (int i = 0; i < 16; ++i) {
        EXPECT_TRUE(std::isfinite(m[i])) << "i=" << i;
    }
}
