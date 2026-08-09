#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <thread>

#include "earth_engine/core/async/AsyncSystem.h"
#include "earth_engine/platform/bridge/PlatformBridge.h"
#include "earth_engine/renderer/TerrainPageStore.h"
#include "earth_engine/tiling/RasterOverlayProjection.h"
#include "earth_engine/tiling/TileScheme.h"
#include "../../helpers/MockRenderDevice.h"

using namespace earth_engine;
using earth_engine::testing::MockRenderDevice;

namespace {

// blockLayers=16(gridN=4), 3 块 → totalLayers=48。
TerrainPageLayerPool makePool(int blockCount = 3, int blockLayers = 16) {
    TerrainPageLayerPool pool;
    pool.configure(blockCount, blockLayers);
    return pool;
}

// side²×4 的单色 RGBA8 页。
std::vector<uint8_t> solidPage(int side, uint8_t r, uint8_t g, uint8_t b,
                               uint8_t a) {
    std::vector<uint8_t> px(static_cast<size_t>(side) * side * 4u);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = a;
    }
    return px;
}

}  // namespace

// ---------------- PageSourceAssembler(C-1 多源按序合成)----------------

// 单源时必须逐字节等价于 C-1 之前(首源直接拷贝,不走 alphaOver 的浮点往返)。
// 这是「页存储改成多源」这次改造的零回归闸:底图独占时画面不能有任何变化。
TEST(PageSourceAssembler, SingleSourceIsByteIdenticalCopy) {
    PageSourceAssembler asmb;
    asmb.configure(1, 2);
    const std::vector<uint8_t> src = solidPage(2, 13, 200, 77, 255);
    EXPECT_TRUE(asmb.accept(0, src.data()));
    EXPECT_TRUE(asmb.hasTexels());
    EXPECT_TRUE(asmb.complete());
    EXPECT_EQ(asmb.texels(), src);
}

// 有序合成:源 1 半透明叠在源 0 上。直通 alpha 的 source-over。
TEST(PageSourceAssembler, CompositesInSourceOrder) {
    PageSourceAssembler asmb;
    asmb.configure(2, 1);
    const std::vector<uint8_t> base = solidPage(1, 0, 0, 0, 255);
    const std::vector<uint8_t> top = solidPage(1, 255, 255, 255, 128);
    ASSERT_TRUE(asmb.accept(0, base.data()));
    ASSERT_TRUE(asmb.accept(1, top.data()));
    ASSERT_TRUE(asmb.complete());
    // sa=128/255≈0.502 → rgb = 255*0.502 + 0*0.498 ≈ 128;a 仍满。
    EXPECT_NEAR(asmb.texels()[0], 128, 1);
    EXPECT_EQ(asmb.texels()[3], 255);
}

// **顺序不可交换**:反序合成必须得到不同结果 —— 这正是「必须按 overlay 序」的理由,
// 也是防止有人日后把 accept 改成「谁先到谁先合成」的钉子。
TEST(PageSourceAssembler, OrderMattersOpaqueTopWins) {
    const std::vector<uint8_t> black = solidPage(1, 0, 0, 0, 255);
    const std::vector<uint8_t> white = solidPage(1, 255, 255, 255, 255);
    PageSourceAssembler a, b;
    a.configure(2, 1);
    a.accept(0, black.data());
    a.accept(1, white.data());
    b.configure(2, 1);
    b.accept(0, white.data());
    b.accept(1, black.data());
    EXPECT_EQ(a.texels()[0], 255);
    EXPECT_EQ(b.texels()[0], 0);
}

// 乱序早到的源必须暂存、不上传;前序补齐后一次性连着消化。
// (否则矢量层比底图先到就会把底图叠在自己上面 → 矢量被盖掉。)
TEST(PageSourceAssembler, OutOfOrderArrivalIsStashedThenDrained) {
    PageSourceAssembler asmb;
    asmb.configure(2, 1);
    const std::vector<uint8_t> base = solidPage(1, 0, 0, 0, 255);
    const std::vector<uint8_t> top = solidPage(1, 255, 255, 255, 255);
    EXPECT_FALSE(asmb.accept(1, top.data())) << "源1先到 → 暂存,不该上传";
    EXPECT_FALSE(asmb.hasTexels());
    EXPECT_TRUE(asmb.accept(0, base.data())) << "源0到齐 → 连带消化源1";
    EXPECT_TRUE(asmb.complete());
    EXPECT_EQ(asmb.texels()[0], 255) << "最终结果与顺序到达一致";
}

// 部分到达先点亮:源 0 到了就该上传(hasTexels),不等最慢的源。
TEST(PageSourceAssembler, PartialArrivalUploadsEarly) {
    PageSourceAssembler asmb;
    asmb.configure(3, 1);
    const std::vector<uint8_t> base = solidPage(1, 10, 20, 30, 255);
    EXPECT_TRUE(asmb.accept(0, base.data()));
    EXPECT_TRUE(asmb.hasTexels());
    EXPECT_FALSE(asmb.complete());
    EXPECT_EQ(asmb.compositedCount(), 1);
}

// 重复到达幂等:同一源第二次必须被丢弃(否则重复 fetch 会把同一层叠两遍,
// 半透明源越叠越浓),且不浪费本帧上传预算。
TEST(PageSourceAssembler, DuplicateArrivalIsIdempotent) {
    PageSourceAssembler asmb;
    asmb.configure(2, 1);
    const std::vector<uint8_t> base = solidPage(1, 0, 0, 0, 255);
    const std::vector<uint8_t> top = solidPage(1, 255, 255, 255, 128);
    ASSERT_TRUE(asmb.accept(0, base.data()));
    ASSERT_TRUE(asmb.accept(1, top.data()));
    const uint8_t after = asmb.texels()[0];
    EXPECT_FALSE(asmb.accept(1, top.data()));
    EXPECT_FALSE(asmb.accept(0, base.data()));
    EXPECT_EQ(asmb.texels()[0], after);
}

// 全源到齐后释放缓冲 → 稳态零额外内存;但已合成的事实不能丢
// (determination 靠 hasTexels 判 cell resident,释放后判错就整片回落 mappedRaster)。
TEST(PageSourceAssembler, ReleaseBuffersKeepsProgressFlags) {
    PageSourceAssembler asmb;
    asmb.configure(1, 4);
    const std::vector<uint8_t> src = solidPage(4, 1, 2, 3, 4);
    ASSERT_TRUE(asmb.accept(0, src.data()));
    asmb.releaseBuffers();
    EXPECT_TRUE(asmb.texels().empty());
    EXPECT_TRUE(asmb.hasTexels());
    EXPECT_TRUE(asmb.complete());
}

// 越界 / 空指针 / 未 configure 一律安全 no-op。
TEST(PageSourceAssembler, RejectsInvalidInput) {
    const std::vector<uint8_t> src = solidPage(1, 9, 9, 9, 255);
    PageSourceAssembler unconfigured;
    EXPECT_FALSE(unconfigured.accept(0, src.data()));

    PageSourceAssembler asmb;
    asmb.configure(2, 1);
    EXPECT_FALSE(asmb.accept(-1, src.data()));
    EXPECT_FALSE(asmb.accept(2, src.data()));
    EXPECT_FALSE(asmb.accept(0, nullptr));
    EXPECT_FALSE(asmb.hasTexels());
}

// 全透明源叠在全透明上 → 仍全透明,且 rgb 确定(不能因除零出 NaN/垃圾)。
TEST(PageSourceAssembler, TransparentOverTransparentStaysZero) {
    PageSourceAssembler asmb;
    asmb.configure(2, 1);
    const std::vector<uint8_t> empty = solidPage(1, 200, 200, 200, 0);
    ASSERT_TRUE(asmb.accept(0, empty.data()));
    ASSERT_TRUE(asmb.accept(1, empty.data()));
    EXPECT_EQ(asmb.texels()[3], 0);
    EXPECT_EQ(asmb.texels()[0], 0);
}

// ---------------- resamplePageSource(C-1b per-source 祖先 scale-bias)-------

namespace {

DecodedImage makeImage(int side, int channels,
                       const std::function<void(int, int, uint8_t*)>& fill) {
    DecodedImage img;
    img.width = side;
    img.height = side;
    img.channels = channels;
    img.bytesPerChannel = 1;
    img.pixels.assign(static_cast<size_t>(side) * side * channels, 0);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            fill(x, y, img.pixels.data() +
                           (static_cast<size_t>(y) * side + x) * channels);
        }
    }
    return img;
}

}  // namespace

// depth=0 且尺寸相同 → 必须逐字节直拷。这是 C-1b 对已有(与页同级)源的零回归闸:
// 0.5 偏移相消让双线性权重恰好为 0,不该引入任何浮点往返误差。
TEST(ResamplePageSource, DepthZeroIsByteIdenticalCopy) {
    const int side = 8;
    DecodedImage img = makeImage(side, 4, [](int x, int y, uint8_t* p) {
        p[0] = static_cast<uint8_t>(x * 31);
        p[1] = static_cast<uint8_t>(y * 17);
        p[2] = static_cast<uint8_t>(x + y);
        p[3] = 255;
    });
    std::vector<uint8_t> out;
    TerrainPageStore::resamplePageSource(img, 0, 0, 0, side, out);
    EXPECT_EQ(out, img.pixels);
}

// depth=1 的四个格位必须各取自己那一象限 —— 取错象限的话真机表现是「路网整体偏移
// 半个瓦片」,肉眼极难反推。用四象限纯色图逐格位钉死。
TEST(ResamplePageSource, PicksCorrectSubQuadrant) {
    const int side = 4;
    // 左上=10 右上=20 左下=30 右下=40(R 通道编号)。
    DecodedImage img = makeImage(side, 4, [side](int x, int y, uint8_t* p) {
        const bool right = x >= side / 2;
        const bool bottom = y >= side / 2;
        p[0] = static_cast<uint8_t>(bottom ? (right ? 40 : 30)
                                           : (right ? 20 : 10));
        p[3] = 255;
    });
    struct Case { int subX, subY, expect; };
    // TileKey y 与影像 row 同向(NW 约定,row0=北)→ subY=0 取上半。
    for (const Case& c : {Case{0, 0, 10}, Case{1, 0, 20},
                          Case{0, 1, 30}, Case{1, 1, 40}}) {
        std::vector<uint8_t> out;
        TerrainPageStore::resamplePageSource(img, 1, c.subX, c.subY, side, out);
        // 取象限中心避开双线性在象限边界的过渡带。
        const size_t mid =
            (static_cast<size_t>(side / 2) * side + side / 2) * 4u;
        EXPECT_EQ(out[mid], c.expect)
            << "sub=(" << c.subX << "," << c.subY << ")";
    }
}

// 纯色祖先放大后仍是同一纯色(不因边界 clamp 或权重归一化跑偏)。
TEST(ResamplePageSource, SolidAncestorUpsamplesFlat) {
    const int side = 8;
    DecodedImage img = makeImage(side, 4, [](int, int, uint8_t* p) {
        p[0] = 90; p[1] = 140; p[2] = 200; p[3] = 255;
    });
    std::vector<uint8_t> out;
    TerrainPageStore::resamplePageSource(img, 3, 5, 2, side, out);
    for (size_t i = 0; i < out.size(); i += 4) {
        ASSERT_EQ(out[i], 90) << "i=" << i;
        ASSERT_EQ(out[i + 1], 140);
        ASSERT_EQ(out[i + 2], 200);
        ASSERT_EQ(out[i + 3], 255);
    }
}

// 3 通道源补满 alpha=255,单通道源铺灰度 —— 否则矢量/标注类源会因 alpha=0 被
// alphaOver 当成「什么都没有」而整层失效。
TEST(ResamplePageSource, FillsAlphaForThreeAndOneChannel) {
    const int side = 2;
    DecodedImage rgb = makeImage(side, 3, [](int, int, uint8_t* p) {
        p[0] = 7; p[1] = 8; p[2] = 9;
    });
    std::vector<uint8_t> out;
    TerrainPageStore::resamplePageSource(rgb, 0, 0, 0, side, out);
    EXPECT_EQ(out[0], 7);
    EXPECT_EQ(out[3], 255);

    DecodedImage gray = makeImage(side, 1, [](int, int, uint8_t* p) {
        p[0] = 123;
    });
    TerrainPageStore::resamplePageSource(gray, 0, 0, 0, side, out);
    EXPECT_EQ(out[0], 123);
    EXPECT_EQ(out[1], 123);
    EXPECT_EQ(out[2], 123);
    EXPECT_EQ(out[3], 255);
}

// 源尺寸与页边长不等时必须重采样而不是被丢弃。真机踩过:矢量源 tileSize=512、
// 页边长 256,旧护栏「非 256² 跳过」静默丢弃了它 —— 图非空所以连错误日志都没有。
TEST(ResamplePageSource, SourceLargerThanPageIsDownsampledNotDropped) {
    const int srcSide = 8;
    const int pageSide = 4;
    DecodedImage img = makeImage(srcSide, 4, [](int, int, uint8_t* p) {
        p[0] = 200; p[1] = 100; p[2] = 50; p[3] = 255;
    });
    std::vector<uint8_t> out;
    TerrainPageStore::resamplePageSource(img, 0, 0, 0, pageSide, out);
    ASSERT_EQ(out.size(), static_cast<size_t>(pageSide) * pageSide * 4u);
    EXPECT_EQ(out[0], 200);
    EXPECT_EQ(out[3], 255);
}

// 空图/非法尺寸 → 全零输出(不越界、不留脏数据)。
TEST(ResamplePageSource, EmptyImageYieldsZeroedOutput) {
    DecodedImage empty;
    std::vector<uint8_t> out;
    TerrainPageStore::resamplePageSource(empty, 0, 0, 0, 4, out);
    ASSERT_EQ(out.size(), 4u * 4u * 4u);
    for (uint8_t v : out) EXPECT_EQ(v, 0);
}

// ---------------- TerrainPageLayerPool(纯 CPU 分配器)----------------

TEST(TerrainPageLayerPool, AcquireGivesContiguousDistinctBlocks) {
    TerrainPageLayerPool pool = makePool(3, 16);
    uint64_t ev = 123;
    EXPECT_EQ(pool.acquire(/*key=*/10, /*frame=*/1, &ev), 0);
    EXPECT_EQ(ev, 0u);
    EXPECT_EQ(pool.acquire(11, 1, &ev), 16);
    EXPECT_EQ(pool.acquire(12, 1, &ev), 32);
    EXPECT_EQ(pool.residentCount(), 3);
    // 各块 layerBase 连续、块尺寸 16。
    EXPECT_EQ(pool.blockLayers(), 16);
    EXPECT_EQ(pool.blockCount(), 3);
}

TEST(TerrainPageLayerPool, ReacquireResidentReturnsSameBaseNoEvict) {
    TerrainPageLayerPool pool = makePool(3, 16);
    uint64_t ev = 0;
    const int base = pool.acquire(10, 1, &ev);
    // 已驻留:同 key 再取返回同 base、不淘汰。
    EXPECT_EQ(pool.acquire(10, 2, &ev), base);
    EXPECT_EQ(ev, 0u);
    EXPECT_EQ(pool.residentCount(), 1);
    EXPECT_EQ(pool.layerBaseFor(10), base);
    EXPECT_EQ(pool.layerBaseFor(999), -1);  // 未驻留
}

TEST(TerrainPageLayerPool, EvictsLeastRecentlyUsedWhenFull) {
    TerrainPageLayerPool pool = makePool(3, 16);
    uint64_t ev = 0;
    pool.acquire(10, /*frame=*/1, &ev);  // slot0
    pool.acquire(11, /*frame=*/2, &ev);  // slot1
    pool.acquire(12, /*frame=*/3, &ev);  // slot2
    // 第 4 个(frame=4):池满 → 淘汰 lastFrame 最小者(key 10,frame 1)。
    const int base = pool.acquire(13, 4, &ev);
    EXPECT_EQ(ev, 10u);
    EXPECT_EQ(base, 0);  // 复用 slot0 的 base
    EXPECT_EQ(pool.layerBaseFor(10), -1);  // 被淘汰
    EXPECT_EQ(pool.layerBaseFor(13), 0);
    EXPECT_EQ(pool.residentCount(), 3);
}

TEST(TerrainPageLayerPool, TouchUpdatesRecencyProtectsFromEviction) {
    TerrainPageLayerPool pool = makePool(3, 16);
    uint64_t ev = 0;
    pool.acquire(10, 1, &ev);
    pool.acquire(11, 2, &ev);
    pool.acquire(12, 3, &ev);
    // frame 4:touch key 10(它本是最久),使 11 变最久。
    pool.acquire(10, 4, &ev);
    EXPECT_EQ(ev, 0u);
    // frame 5:池满 → 现最久是 key 11(frame 2)。
    pool.acquire(13, 5, &ev);
    EXPECT_EQ(ev, 11u);
    EXPECT_EQ(pool.layerBaseFor(10), 0);   // 因 touch 存活
    EXPECT_EQ(pool.layerBaseFor(11), -1);  // 被淘汰
}

// touch():驻留 key → 更新 recency 保活(祖先回退用);不驻留 key → no-op,
// **不分配、不淘汰**(区别于 acquire)。§16.4 缺页祖先回退依赖此语义。
TEST(TerrainPageLayerPool, TouchMethodProtectsResidentAndNoopsAbsent) {
    TerrainPageLayerPool pool = makePool(3, 16);
    uint64_t ev = 0;
    pool.acquire(10, 1, &ev);
    pool.acquire(11, 2, &ev);
    pool.acquire(12, 3, &ev);
    // 不驻留 key → no-op:不占块、不淘汰、residentCount 不变。
    pool.touch(999, 4);
    EXPECT_EQ(pool.residentCount(), 3);
    EXPECT_EQ(pool.layerBaseFor(999), -1);
    // 驻留 key 10(本最久)→ touch 保活,使 11 变最久。
    pool.touch(10, 5);
    pool.acquire(13, 6, &ev);  // 池满 → 淘汰现最久 key 11
    EXPECT_EQ(ev, 11u);
    EXPECT_EQ(pool.layerBaseFor(10), 0);   // 因 touch 存活
    EXPECT_EQ(pool.layerBaseFor(11), -1);  // 被淘汰
}

TEST(TerrainPageLayerPool, RefusesEvictionWhenAllTouchedThisFrame) {
    TerrainPageLayerPool pool = makePool(2, 16);
    uint64_t ev = 0;
    pool.acquire(10, /*frame=*/7, &ev);  // 本帧
    pool.acquire(11, /*frame=*/7, &ev);  // 本帧
    // 第三个瓦片同帧:两块都是本帧可见 → 不淘汰,返回 -1(调用方回落 mappedRaster)。
    const int base = pool.acquire(12, 7, &ev);
    EXPECT_EQ(base, -1);
    EXPECT_EQ(ev, 0u);
    EXPECT_EQ(pool.residentCount(), 2);
}

TEST(TerrainPageLayerPool, ReleaseFreesBlockForReuse) {
    TerrainPageLayerPool pool = makePool(2, 16);
    uint64_t ev = 0;
    const int b0 = pool.acquire(10, 1, &ev);
    pool.acquire(11, 1, &ev);
    pool.release(10);
    EXPECT_EQ(pool.layerBaseFor(10), -1);
    EXPECT_EQ(pool.residentCount(), 1);
    // 释放后空块可再分配(同帧也行,因不需淘汰)。
    const int reused = pool.acquire(12, 1, &ev);
    EXPECT_EQ(reused, b0);
    EXPECT_EQ(ev, 0u);
    pool.release(999);  // 不存在 → no-op
}

// B2b 页粒度:blockLayers=1 → layerBase == slot index(每页一层)。LRU 逐页淘汰。
TEST(TerrainPageLayerPool, PageGranularBlockLayersOne) {
    TerrainPageLayerPool pool;
    pool.configure(/*blockCount=*/4, /*blockLayers=*/1);
    EXPECT_EQ(pool.blockLayers(), 1);
    uint64_t ev = 0;
    // 每页认领一层,layer == slot index(连续 0,1,2,3)。
    EXPECT_EQ(pool.acquire(/*key=*/100, /*frame=*/1, &ev), 0);
    EXPECT_EQ(pool.acquire(101, 1, &ev), 1);
    EXPECT_EQ(pool.acquire(102, 1, &ev), 2);
    EXPECT_EQ(pool.acquire(103, 1, &ev), 3);
    EXPECT_EQ(pool.residentCount(), 4);
    // 已驻留页复取返回同层、不淘汰。
    EXPECT_EQ(pool.acquire(101, 2, &ev), 1);
    EXPECT_EQ(ev, 0u);
    // 池满 + 新页(frame 3):淘汰 lastFrame 最小者(key 100,frame 1),复用其层 0。
    const int reused = pool.acquire(200, 3, &ev);
    EXPECT_EQ(ev, 100u);
    EXPECT_EQ(reused, 0);
    EXPECT_EQ(pool.layerBaseFor(100), -1);  // 页被淘汰
    EXPECT_EQ(pool.layerBaseFor(200), 0);
}

TEST(TerrainPageLayerPool, ConfigureResetsResidency) {
    TerrainPageLayerPool pool = makePool(3, 16);
    uint64_t ev = 0;
    pool.acquire(10, 1, &ev);
    pool.configure(2, 4);  // 重配 → 清空
    EXPECT_EQ(pool.residentCount(), 0);
    EXPECT_EQ(pool.blockCount(), 2);
    EXPECT_EQ(pool.blockLayers(), 4);
    EXPECT_EQ(pool.layerBaseFor(10), -1);
}

// ---------------- TerrainPageStore(创建/门控)----------------

TEST(TerrainPageStore, InitFailsOnNullDevice) {
    TerrainPageStore store;
    EXPECT_FALSE(store.initialize(nullptr, TerrainPageStore::Config{}));
    EXPECT_FALSE(store.isReady());
}

TEST(TerrainPageStore, InitializeCreatesSharedArrayTexture) {
    MockRenderDevice device;
    TerrainPageStore store;
    TerrainPageStore::Config cfg;
    cfg.maxPages = 64;  // B2b:每页一层 → array 层数 = maxPages
    ASSERT_TRUE(store.initialize(&device, cfg));
    EXPECT_TRUE(store.isReady());
    // 合批 Step 2:页存储 array + 间接纹理共享 array 各一张(后创建的
    // indir array 是 lastTextureDesc:固定 64² × kIndirArrayLayers 层)。
    EXPECT_EQ(device.createdTextureCount, 2);
    EXPECT_EQ(device.lastTextureDesc.arrayLayers,
              TerrainPageStore::kIndirArrayLayers);
    EXPECT_EQ(device.lastTextureDesc.width,
              TerrainPageStore::kIndirSideTexels);
    EXPECT_EQ(store.residentPageCount(), 0);
    EXPECT_EQ(store.uploadedLayerTotal(), 0);
}

// packKey/unpackKey 必须 round-trip:叠画钩子拿到的 z/x/y 错了就会去取错误的
// 源瓦片,画面表现是「路网整体错位」而不是报错。覆盖到 z17 与各位段边界。
TEST(TerrainPageStore, PackUnpackKeyRoundTrip) {
    for (const TileKey& key : {TileKey{"", 0, 0, 0}, TileKey{"", 17, 0, 0},
                               TileKey{"", 17, 131071, 131071},
                               TileKey{"", 12, 3260, 1695}}) {
        const TileKey back =
            TerrainPageStore::unpackKey(TerrainPageStore::packKeyForTest(key));
        EXPECT_EQ(back.z, key.z);
        EXPECT_EQ(back.x, key.x);
        EXPECT_EQ(back.y, key.y);
    }
}

// ---------------- SVT 间接纹理 RGBA8 层编解码(Step B1)----------------

// 编 layer → RGBA8 → 解码回 layer,逐位镜像片元 shader(R+G*256)。decode 只看 RG,
// resident(A)不影响 layer 还原 → 两种 resident 都应 round-trip。
// 覆盖边界:0(全零)/255(R 满)/256(进 G 位)/511(R 满+G=1)/大值(双通道)。
TEST(TerrainPageStoreIndir, EncodeDecodeRoundTrip) {
    for (int layer : {0, 1, 127, 254, 255, 256, 257, 511, 512,
                      1000, 4095, 4096, 12345, 65535}) {
        for (bool resident : {true, false}) {
            for (int depth : {0, 1, 3, 6}) {  // §16.3:d 独立于 layer round-trip
                uint8_t rgba[4] = {0, 0, 0, 0};
                TerrainPageStore::encodeLayerRGBA8(layer, resident, depth, rgba);
                EXPECT_EQ(TerrainPageStore::decodeLayerRGBA8(rgba), layer)
                    << "layer=" << layer << " resident=" << resident
                    << " depth=" << depth;
                EXPECT_EQ(TerrainPageStore::decodeDepthRGBA8(rgba), depth)
                    << "layer=" << layer << " depth=" << depth;
            }
        }
    }
}

// §16.3:depth clamp 到 [0, kMaxDetDepthLevels]=6(B 通道容 0..255,但语义 ≤6)。
// 负数 → 0,超 6 → 6;解码回值 = clamp 后。layer 编码不受 depth 影响。
TEST(TerrainPageStoreIndir, DepthClampToMaxDetDepth) {
    uint8_t rgba[4] = {0, 0, 0, 0};
    TerrainPageStore::encodeLayerRGBA8(42, /*resident=*/true, /*depth=*/-3, rgba);
    EXPECT_EQ(TerrainPageStore::decodeDepthRGBA8(rgba), 0);
    EXPECT_EQ(TerrainPageStore::decodeLayerRGBA8(rgba), 42);
    TerrainPageStore::encodeLayerRGBA8(42, /*resident=*/true, /*depth=*/6, rgba);
    EXPECT_EQ(TerrainPageStore::decodeDepthRGBA8(rgba), 6);
    TerrainPageStore::encodeLayerRGBA8(42, /*resident=*/true, /*depth=*/99, rgba);
    EXPECT_EQ(TerrainPageStore::decodeDepthRGBA8(rgba), 6);  // clamp 到 6
}

// 编码约定:R=layer&0xFF、G=(layer>>8)&0xFF、B=depth(§16.3)、A=resident?255:0。
TEST(TerrainPageStoreIndir, EncodeChannelLayout) {
    uint8_t rgba[4] = {9, 9, 9, 9};
    TerrainPageStore::encodeLayerRGBA8(0, /*resident=*/true, /*depth=*/0, rgba);
    EXPECT_EQ(rgba[0], 0);
    EXPECT_EQ(rgba[1], 0);
    EXPECT_EQ(rgba[2], 0);    // B = depth 0
    EXPECT_EQ(rgba[3], 255);  // A = resident → 255

    // B 通道 = depth(渐变 LOD 级数),独立于 layer/resident。
    TerrainPageStore::encodeLayerRGBA8(0, /*resident=*/true, /*depth=*/2, rgba);
    EXPECT_EQ(rgba[2], 2);

    // A 通道 = resident 标志:miss(resident=false)→ A=0,片元 alphaOver factor=0。
    TerrainPageStore::encodeLayerRGBA8(0, /*resident=*/false, /*depth=*/0, rgba);
    EXPECT_EQ(rgba[3], 0);

    TerrainPageStore::encodeLayerRGBA8(255, /*resident=*/true, /*depth=*/0, rgba);
    EXPECT_EQ(rgba[0], 255);  // R 满
    EXPECT_EQ(rgba[1], 0);

    TerrainPageStore::encodeLayerRGBA8(256, /*resident=*/true, /*depth=*/0, rgba);
    EXPECT_EQ(rgba[0], 0);    // R 归零
    EXPECT_EQ(rgba[1], 1);    // 进 G 位

    TerrainPageStore::encodeLayerRGBA8(513, /*resident=*/true, /*depth=*/0, rgba);
    EXPECT_EQ(rgba[0], 1);    // 513 = 1 + 2*256
    EXPECT_EQ(rgba[1], 2);
}

// 解码逐位镜像片元 shader:floor(r*255+0.5)+floor(g*255+0.5)*256。
// 直接用「采样后的 unorm 值」路径(byte/255→*255→floor+0.5)验与整数解码一致。
TEST(TerrainPageStoreIndir, DecodeMirrorsShaderMath) {
    for (int R = 0; R <= 255; R += 17) {
        for (int G = 0; G <= 255; G += 51) {
            const uint8_t rgba[4] = {static_cast<uint8_t>(R),
                                     static_cast<uint8_t>(G), 0, 255};
            const int viaHelper = TerrainPageStore::decodeLayerRGBA8(rgba);
            // shader 路径:unorm 采样 r=R/255,floor(r*255+0.5)=R。
            const float rf = static_cast<float>(R) / 255.0f;
            const float gf = static_cast<float>(G) / 255.0f;
            const int viaShader =
                static_cast<int>(std::floor(rf * 255.0f + 0.5f)) +
                static_cast<int>(std::floor(gf * 255.0f + 0.5f)) * 256;
            EXPECT_EQ(viaHelper, viaShader) << "R=" << R << " G=" << G;
            EXPECT_EQ(viaHelper, R + G * 256);
        }
    }
}

TEST(TerrainPageStore, TickBeforeAnyTileIsNoop) {
    MockRenderDevice device;
    TerrainPageStore store;
    ASSERT_TRUE(store.initialize(&device, TerrainPageStore::Config{}));
    store.tick();  // 无 entry、无 provider → 不崩、无上传
    EXPECT_EQ(store.uploadedLayerTotal(), 0);
}

// ---------------- 门② determination 纯函数(Step B2a)----------------

// gridN = sourceZoom ≤ tileZ ? 1 : 1<<(sourceZoom-tileZ)。
TEST(TerrainPageDet, SubtileGridNFormula) {
    EXPECT_EQ(TerrainPageStore::subtileGridN(12, 12), 1);  // 相等 → 不细分
    EXPECT_EQ(TerrainPageStore::subtileGridN(12, 11), 1);  // 源更浅 → guard 1
    EXPECT_EQ(TerrainPageStore::subtileGridN(12, 13), 2);
    EXPECT_EQ(TerrainPageStore::subtileGridN(12, 14), 4);
    EXPECT_EQ(TerrainPageStore::subtileGridN(12, 16), 16);  // 近景 z16
    EXPECT_EQ(TerrainPageStore::subtileGridN(12, 17), 32);  // 近景 z17
    EXPECT_EQ(TerrainPageStore::subtileGridN(0, 3), 8);
}

// 子瓦片 key = mercator 直接子瓦片:(z=sourceZoom, x=tileX*gridN+dx, y=tileY*gridN+dy)。
TEST(TerrainPageDet, EnumerateSubtileKeysAligned) {
    TileKey parent;
    parent.z = 12;
    parent.x = 3;
    parent.y = 5;
    std::vector<TileKey> out;
    TerrainPageStore::enumerateSubtileKeys(parent, /*sourceZoom=*/14, out);
    ASSERT_EQ(out.size(), 16u);  // gridN=4 → 4×4
    // 全部 z=14,schemeId 沿用,x∈[12,15]、y∈[20,23](= parent*4 + [0,3])。
    for (const TileKey& k : out) {
        EXPECT_EQ(k.z, 14);
        EXPECT_EQ(k.schemeId, parent.schemeId);
        EXPECT_GE(k.x, 12);
        EXPECT_LE(k.x, 15);
        EXPECT_GE(k.y, 20);
        EXPECT_LE(k.y, 23);
    }
    // 枚举序 dy 外层、dx 内层(与 shader cell.y*gridN+cell.x + kickImageryFetch 一致):
    // 首格 (dy=0,dx=0) = 西北角 (x=12,y=20);末格 = (x=15,y=23)。
    EXPECT_EQ(out.front().x, 12);
    EXPECT_EQ(out.front().y, 20);
    EXPECT_EQ(out.back().x, 15);
    EXPECT_EQ(out.back().y, 23);
    // 第 5 格(index 4)= dy=1,dx=0 → (x=12,y=21)。
    EXPECT_EQ(out[4].x, 12);
    EXPECT_EQ(out[4].y, 21);
}

// sourceZoom == tileZ → gridN 1 → 单个 key 即瓦片自身。
TEST(TerrainPageDet, EnumerateSubtileKeysNoSubdivision) {
    TileKey parent;
    parent.z = 12;
    parent.x = 7;
    parent.y = 9;
    std::vector<TileKey> out;
    TerrainPageStore::enumerateSubtileKeys(parent, /*sourceZoom=*/12, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].z, 12);
    EXPECT_EQ(out[0].x, 7);
    EXPECT_EQ(out[0].y, 9);
}

// ---------------------------------------------------------------------------
// 合成下 worker 的并发正确性:mutex 序列化 + stash 兜乱序,任务执行次序无关。
// 判据 = 与单线程顺序合成逐字节等价(alphaOver 不可交换,错序会当场翻脸)。
// 这就是 .cpp 里 PageComposeState 的并发形态(mutex+assembler),在此直接
// 用同构组合测,不需要 GL 设备夹具。
// ---------------------------------------------------------------------------

TEST(PageSourceAssembler, ConcurrentOutOfOrderAcceptMatchesSequential) {
    constexpr int kSide = 16;
    constexpr int kSources = 4;
    const size_t bytes = static_cast<size_t>(kSide) * kSide * 4u;

    // 每源一块半透明纯色(alpha 176):层序不同结果必不同 → 等价断言有判别力。
    std::vector<std::vector<uint8_t>> sources;
    for (int s = 0; s < kSources; ++s) {
        std::vector<uint8_t> img(bytes);
        for (size_t i = 0; i < bytes; i += 4) {
            img[i + 0] = static_cast<uint8_t>(40 * (s + 1));
            img[i + 1] = static_cast<uint8_t>(255 - 50 * s);
            img[i + 2] = static_cast<uint8_t>(17 * (s + 3));
            img[i + 3] = 176;
        }
        sources.push_back(std::move(img));
    }

    // 基准:单线程按序合成。
    PageSourceAssembler sequential;
    sequential.configure(kSources, kSide);
    for (int s = 0; s < kSources; ++s) {
        sequential.accept(s, sources[static_cast<size_t>(s)].data());
    }
    ASSERT_TRUE(sequential.complete());
    const std::vector<uint8_t> expected = sequential.texels();

    // 被测:4 worker 乱序并发提交(含重复提交,验幂等),外置 mutex 序列化。
    for (int round = 0; round < 8; ++round) {
        PageSourceAssembler shared;
        shared.configure(kSources, kSide);
        std::mutex mutex;
        ThreadPool pool(4);
        std::atomic<int> pendingTasks{0};
        const int order[] = {3, 1, 0, 2, 1, 3};  // 乱序 + 两个重复
        for (int s : order) {
            pendingTasks.fetch_add(1);
            pool.enqueue([&shared, &mutex, &sources, &pendingTasks, s]() {
                std::lock_guard<std::mutex> lock(mutex);
                shared.accept(s, sources[static_cast<size_t>(s)].data());
                pendingTasks.fetch_sub(1);
            });
        }
        for (int spin = 0; spin < 2000 && pendingTasks.load() > 0; ++spin) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(0, pendingTasks.load());
        ASSERT_TRUE(shared.complete());
        EXPECT_EQ(expected, shared.texels()) << "round=" << round;
    }
}

// ───────── D 方案:cell 网格 = 影像源瓦片网格 ─────────
//
// 页存储原本用 `tileKey.x*gridN+dx` 直接推源瓦片 key,等于假设几何网格 == 源网格。
// GCJ-02 底图的源瓦片以偏移坐标编址,这个假设破裂 —— 真机上表现为高德影像完全不被
// 校正(20× 放大偏移屏幕仍逐像素不变)。placeTileInSourceGrid 把 cell 网格重定义成
// 源瓦片网格,错位由 origin/span 承载。

namespace {

Rectangle mercatorTileRect(const TileScheme& scheme, int z, int x, int y) {
    return projectRasterSourceRectangle(
        scheme.tileToRectangle(TileKey{scheme.id(), z, x, y}),
        RasterOverlayProjection::WebMercator);
}

} // namespace

// 标准 overlay 必须逐字段退化成改造前的 `uv*gridN`。这条是零回归判据:
// 它红 = D 改动漏进了本不该生效的路径。
TEST(TerrainPageStorePlacement, StandardOverlayDegeneratesToGeometryGrid) {
    auto scheme = TileScheme::createXYZWebMercator();
    for (int depth = 0; depth <= 3; ++depth) {
        const int gridN = 1 << depth;
        const int tileZ = 12;
        const int tileX = 3260;
        const int tileY = 1694;
        const Rectangle details = mercatorTileRect(*scheme, tileZ, tileX, tileY);
        const TerrainPageStore::SourceTilePlacement placement =
            TerrainPageStore::placeTileInSourceGrid(
                *scheme,
                details,
                RasterOverlayProjection::WebMercator,
                tileZ + depth,
                gridN);

        EXPECT_TRUE(placement.isDegenerate(gridN))
            << "depth=" << depth
            << " cells=" << placement.cellsX << "x" << placement.cellsY
            << " origin=" << placement.originU << "," << placement.originV
            << " span=" << placement.spanU << "," << placement.spanV;
        EXPECT_EQ(tileX * gridN, placement.x0) << "depth=" << depth;
        EXPECT_EQ(tileY * gridN, placement.y0) << "depth=" << depth;
    }
}

// GCJ 下几何瓦片落在源网格的非整数位置 → 必须多取一列一行,且 origin 非零。
TEST(TerrainPageStorePlacement, Gcj02ShiftsOntoNonIntegerSourcePosition) {
    auto scheme = TileScheme::createXYZWebMercator();
    const int tileZ = 12;
    const int tileX = 3260;   // 重庆
    const int tileY = 1694;
    const int gridN = 4;
    const Rectangle worldRect =
        scheme->tileToRectangle(TileKey{scheme->id(), tileZ, tileX, tileY});
    const Rectangle details = projectWorldRectangleForRasterOverlay(
        worldRect, RasterOverlayProjection::Gcj02WebMercator);

    const TerrainPageStore::SourceTilePlacement placement =
        TerrainPageStore::placeTileInSourceGrid(
            *scheme,
            details,
            RasterOverlayProjection::Gcj02WebMercator,
            tileZ + 2,
            gridN);

    EXPECT_FALSE(placement.isDegenerate(gridN));
    // 平移不足一格 → 覆盖范围恰好多一列一行(不是 4 倍)。
    EXPECT_EQ(gridN + 1, placement.cellsX);
    EXPECT_EQ(gridN + 1, placement.cellsY);
    EXPECT_GT(placement.originU, 0.0);
    EXPECT_GT(placement.originV, 0.0);
    EXPECT_LT(placement.originU, 1.0);
    EXPECT_LT(placement.originV, 1.0);
}

// 核心不变量:片元 t = origin + uv*span 必须落在 [0, cells] 内,且 uv 端点对应的
// 源瓦片就是覆盖 detailsRect 端点的那一张。错一格 = 屏幕上整片错位一整张瓦片。
TEST(TerrainPageStorePlacement, UvMapsIntoCoveringRangeForBothProjections) {
    auto scheme = TileScheme::createXYZWebMercator();
    const int tileZ = 12;
    const int tileY = 1694;
    const RasterOverlayProjection projections[] = {
        RasterOverlayProjection::WebMercator,
        RasterOverlayProjection::Gcj02WebMercator};

    for (RasterOverlayProjection projection : projections) {
        for (int tileX : {3259, 3260, 3261}) {
            const Rectangle worldRect = scheme->tileToRectangle(
                TileKey{scheme->id(), tileZ, tileX, tileY});
            const Rectangle details =
                projectWorldRectangleForRasterOverlay(worldRect, projection);
            const int gridN = 4;
            const TerrainPageStore::SourceTilePlacement p =
                TerrainPageStore::placeTileInSourceGrid(
                    *scheme, details, projection, tileZ + 2, gridN);

            // uv=0 与 uv=1 两端都必须留在覆盖范围内(含边界)。
            EXPECT_GE(p.originU, -1e-9);
            EXPECT_GE(p.originV, -1e-9);
            EXPECT_LE(p.originU + p.spanU,
                      static_cast<double>(p.cellsX) + 1e-9);
            EXPECT_LE(p.originV + p.spanV,
                      static_cast<double>(p.cellsY) + 1e-9);

            // 片元 uv → cell,必须就是覆盖该 uv 对应地面点的那张源瓦片。
            // 采样比例避开 0.5:gridN 为偶数时瓦片中点恰压在 cell 边界上,
            // floor 与 positionToTile 会各走一边 —— 那是采样点选得差,不是错位。
            const Rectangle sourceRect =
                unprojectRasterSourceRectangle(details, projection);
            for (double frac : {0.3, 0.55, 0.8}) {
                const double tU = p.originU + frac * p.spanU;
                const double tV = p.originV + frac * p.spanV;
                const int cellX = p.x0 + static_cast<int>(std::floor(tU));
                const int cellY = p.y0 + static_cast<int>(std::floor(tV));
                // uv 在源经纬空间对应的点(V 走 NW:frac=0 在北)。
                const TileKey expected = scheme->positionToTile(
                    sourceRect.west() + frac * sourceRect.width(),
                    sourceRect.north() -
                        frac * sourceRect.computeHeight(),
                    tileZ + 2);
                EXPECT_EQ(expected.x, cellX)
                    << "proj=" << static_cast<int>(projection)
                    << " x=" << tileX << " frac=" << frac;
                EXPECT_EQ(expected.y, cellY)
                    << "proj=" << static_cast<int>(projection)
                    << " x=" << tileX << " frac=" << frac;
            }
        }
    }
}

// ---- 在途判定必须有终止态 ----
//
// 帧级按需渲染拿这条判"还不能停帧"。页可以长期停在 partial(某个源在该 key 上
// 根本没有数据,partialPages 本就是稳态统计项),"任何一页未完成"那种写法在这类
// 页可见期间恒真 → 渲染循环永远停不下来。失效方向良性(不冻屏、只白烧),但白烧
// 正是 gating 要消灭的东西:没有终止条件的判据 = 把开关焊死在"开"。
// 与 pan 惯性那个 bug 同源。
TEST(TerrainPageStoreInFlight, CompletedPageNeverCountsAsInFlight) {
    EXPECT_FALSE(TerrainPageStore::pageCountsAsInFlight(
        /*uploadComplete=*/true, /*frameId=*/1000, /*lastProgressFrame=*/0));
}

TEST(TerrainPageStoreInFlight, RecentlyProgressingPageCountsAsInFlight) {
    EXPECT_TRUE(TerrainPageStore::pageCountsAsInFlight(false, 1000, 1000));
    EXPECT_TRUE(TerrainPageStore::pageCountsAsInFlight(
        false, 1000, 1000 - TerrainPageStore::kStalledPageFrames));
}

TEST(TerrainPageStoreInFlight, StalledPageStopsCountingAsInFlight) {
    // 这条红了就意味着"某个源永远不到货的页"会把渲染循环永久顶住。
    EXPECT_FALSE(TerrainPageStore::pageCountsAsInFlight(
        false, 1000, 1000 - TerrainPageStore::kStalledPageFrames - 1));
    EXPECT_FALSE(TerrainPageStore::pageCountsAsInFlight(false, 100000, 0));
}

TEST(TerrainPageStoreInFlight, FrameCounterResetIsTreatedAsProgress) {
    // surface 重建把 frameId_ 归零。保守侧:当作刚有进度,宁可多画一帧。
    EXPECT_TRUE(TerrainPageStore::pageCountsAsInFlight(false, 0, 5000));
}
