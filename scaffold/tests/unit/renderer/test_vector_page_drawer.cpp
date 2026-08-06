#include <gtest/gtest.h>

#include "earth_engine/renderer/VectorPageDrawer.h"

#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "earth_engine/renderer/Renderer.h"
#include "../../helpers/MockRenderDevice.h"

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

// ---------------------------------------------------------------------------
// 源瓦片缓存收敛(准入控制 + 未消费保护)
//
// 钉住的缺陷:页 zoom ≤ maxSourceZoom 时页与源瓦片 1:1,可见页数超过
// maxCachedTiles 后,旧实现的「miss 即准入 + 无条件挤掉队尾」会把还没
// 等到 fetch 结果 / 还没服务过页的瓦片换出,结果回来查不到条目直接作废
// → fetch→evict→重拉 永不收敛(真机 z9-10 / 291 页:drawn 恒 0、evict
// 风暴、矢量整层缺失)。
// ---------------------------------------------------------------------------

// 测试侧最小 MVT 编码器(与 test_mvt_decoder.cpp 同源思路:独立实现,
// 不与被测解码器共享代码)。只编一条线要素 —— 够让网格非空即可。
struct PbfWriter {
    std::vector<uint8_t> bytes;
    void varint(uint64_t v) {
        while (v >= 0x80) {
            bytes.push_back(static_cast<uint8_t>(v) | 0x80);
            v >>= 7;
        }
        bytes.push_back(static_cast<uint8_t>(v));
    }
    void tag(uint32_t field, uint32_t wire) { varint((field << 3) | wire); }
    void varintField(uint32_t field, uint64_t v) {
        tag(field, 0);
        varint(v);
    }
    void stringField(uint32_t field, const std::string& s) {
        tag(field, 2);
        varint(s.size());
        bytes.insert(bytes.end(), s.begin(), s.end());
    }
    void bytesField(uint32_t field, const std::vector<uint8_t>& sub) {
        tag(field, 2);
        varint(sub.size());
        bytes.insert(bytes.end(), sub.begin(), sub.end());
    }
    void packedUint32Field(uint32_t field, const std::vector<uint32_t>& vs) {
        PbfWriter payload;
        for (uint32_t v : vs) payload.varint(v);
        bytesField(field, payload.bytes);
    }
};

uint32_t zigzag(int32_t v) {
    return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31);
}

/// 一条对角线 LineString 的最小合法 MVT("roads" 层,extent 4096)。
std::vector<uint8_t> makeLineTileBytes() {
    // MoveTo(0,0) + LineTo(4096,4096)
    std::vector<uint32_t> geom;
    geom.push_back(1u | (1u << 3));
    geom.push_back(zigzag(0));
    geom.push_back(zigzag(0));
    geom.push_back(2u | (1u << 3));
    geom.push_back(zigzag(4096));
    geom.push_back(zigzag(4096));

    PbfWriter feature;
    feature.varintField(1, 1);  // id
    feature.varintField(3, 2);  // type = LineString
    feature.packedUint32Field(4, geom);

    PbfWriter layer;
    layer.varintField(15, 2);  // version
    layer.stringField(1, "roads");
    layer.bytesField(2, feature.bytes);

    PbfWriter tile;
    tile.bytesField(3, layer.bytes);
    return tile.bytes;
}

VectorRasterStyle roadsLineStyle() {
    VectorRasterLayerPaint paint;
    paint.layer = "roads";
    paint.lineColor = {255, 255, 255, 255};
    paint.lineWidthPixels = 2.0;
    VectorRasterStyle style;
    style.layers = {paint};
    return style;
}

/// 收敛测试台:一组同 z 的页(与源 1:1),按固定顺序轮询未叠画的页,
/// fetch 由测试控制完成时机 —— 复刻页存储 retryPendingDecorations 的
/// 每帧预算轮询 + 真实网络的延迟到达。
struct ConvergenceHarness {
    earth_engine::testing::MockRenderDevice device;
    Renderer renderer{&device};
    std::unique_ptr<Texture> target;
    std::unique_ptr<VectorPageDrawer> drawer;

    struct PendingFetch {
        TileKey key;
        std::function<void(int, std::vector<uint8_t>)> done;
    };
    std::vector<PendingFetch> pendingFetches;
    int totalFetches = 0;

    std::vector<TileKey> pages;
    std::vector<bool> decorated;

    explicit ConvergenceHarness(size_t maxCachedTiles, int pageCount) {
        EXPECT_TRUE(renderer.initialize());
        TextureDesc td;
        td.width = 256;
        td.height = 256;
        target = device.createTexture(td);

        VectorPageDrawer::Options options;
        options.style = roadsLineStyle();
        options.maxSourceZoom = 14;
        options.maxCachedTiles = maxCachedTiles;
        drawer = std::make_unique<VectorPageDrawer>(
            &device, &renderer, /*pool=*/nullptr, options,
            [this](const TileKey& key,
                   std::function<void(int, std::vector<uint8_t>)> done) {
                ++totalFetches;
                pendingFetches.push_back({key, std::move(done)});
            });

        for (int i = 0; i < pageCount; ++i) {
            TileKey key;
            key.z = 10;  // < maxSourceZoom → depth=0,页与源 1:1
            key.x = static_cast<uint32_t>(i);
            key.y = 0;
            pages.push_back(key);
        }
        decorated.assign(pages.size(), false);
    }

    int undecoratedCount() const {
        int n = 0;
        for (bool d : decorated) n += d ? 0 : 1;
        return n;
    }

    /// 一帧:tick + 预算内轮询未叠画页。轮询起点逐帧旋转 —— 页存储的
    /// pages_ 是 unordered_map,插入/删除会改变迭代序,每帧被轮到的子集
    /// 并不稳定;固定前缀会让旧实现侥幸收敛,测不出真机形态。
    void frame(int retryBudget) {
        drawer->tickDecorator();
        int budget = retryBudget;
        const size_t start = (frameIndex_++ * 7) % pages.size();
        for (size_t k = 0; k < pages.size() && budget > 0; ++k) {
            const size_t i = (start + k) % pages.size();
            if (decorated[i]) continue;
            --budget;
            decorated[i] = drawer->decoratePage(pages[i], target.get(),
                                                static_cast<int>(i % 64));
        }
    }
    size_t frameIndex_ = 0;

    /// 完成最多 n 个在途 fetch(FIFO,模拟网络陆续到达)。
    void completeFetches(int n, int statusCode,
                         const std::vector<uint8_t>& body) {
        while (n-- > 0 && !pendingFetches.empty()) {
            PendingFetch f = std::move(pendingFetches.front());
            pendingFetches.erase(pendingFetches.begin());
            f.done(statusCode, body);  // pool=null → 解码+建网格就地跑
        }
    }
};

// **收敛 + 恰好一次 fetch**:页数(30)远超缓存容量(8)时,所有页仍在有限
// 帧内叠画完成,且每个源瓦片只被拉取一次 —— 未消费保护保证结果到达前
// 瓦片不被挤出、结果不作废。旧实现在此形态下 fetch 无限增长、drawn 恒 0。
TEST(VectorPageDrawerCache, WideViewConvergesWithEachTileFetchedOnce) {
    ConvergenceHarness h(/*maxCachedTiles=*/8, /*pageCount=*/30);
    const std::vector<uint8_t> body = makeLineTileBytes();

    int frames = 0;
    for (; frames < 800 && h.undecoratedCount() > 0; ++frames) {
        h.frame(/*retryBudget=*/4);
        // 模拟慢网络:每 4 帧才到 2 个结果 —— 在途窗口远大于容量,
        // 旧实现在这个形态下会把还没到货的瓦片挤出去重拉。
        if (frames % 4 == 3) {
            h.completeFetches(2, 200, body);
        }
    }

    EXPECT_EQ(0, h.undecoratedCount()) << "frames=" << frames;
    EXPECT_EQ(30, h.drawer->drawnPageCount());
    // 每瓦片恰好一次:任何重拉都说明有未消费瓦片被挤出过。
    EXPECT_EQ(30, h.totalFetches);
}

// **满员全受保护 → 拒绝准入而不是挤掉在途瓦片**:不完成任何 fetch,
// 容量 2 / 页 5,反复轮询后在途 fetch 恒为 2(旧实现每轮挤掉队尾再重拉,
// fetch 数随帧数无限涨)。
TEST(VectorPageDrawerCache, AdmissionDeferredWhileAllTilesInFlight) {
    ConvergenceHarness h(/*maxCachedTiles=*/2, /*pageCount=*/5);

    for (int f = 0; f < 50; ++f) {
        h.frame(/*retryBudget=*/4);
    }

    EXPECT_EQ(2, h.totalFetches) << "在途瓦片被挤出并重拉了";
    EXPECT_EQ(0, h.drawer->drawnPageCount());

    // 结果到达后系统解锁:全部页最终叠画完成,且总 fetch = 页数。
    const std::vector<uint8_t> body = makeLineTileBytes();
    for (int f = 0; f < 200 && h.undecoratedCount() > 0; ++f) {
        h.completeFetches(2, 200, body);
        h.frame(/*retryBudget=*/4);
    }
    EXPECT_EQ(0, h.undecoratedCount());
    EXPECT_EQ(5, h.totalFetches);
}

// 404(数据覆盖外)也算消费:页判「已处理」不再重试,瓦片可被换出,
// 不占死保护槽位。
TEST(VectorPageDrawerCache, FailedTilesAreConsumedAndEvictable) {
    ConvergenceHarness h(/*maxCachedTiles=*/2, /*pageCount=*/6);

    for (int f = 0; f < 100 && h.undecoratedCount() > 0; ++f) {
        h.frame(/*retryBudget=*/4);
        h.completeFetches(2, 404, {});
    }

    EXPECT_EQ(0, h.undecoratedCount());
    EXPECT_EQ(0, h.drawer->drawnPageCount()) << "404 不该画出东西";
    EXPECT_EQ(6, h.totalFetches);
}
