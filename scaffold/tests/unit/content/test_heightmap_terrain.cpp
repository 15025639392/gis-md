// Heightmap terrain (regular-grid raster DEM, CPU-baked) tests:
//   - P0: HeightmapTerrainProvider::decodeTile decodes Mapbox Terrain-RGB to the
//     correct heights (the encode↔decode contract shared with the data pipeline).
//   - P1: HeightmapTerrainContentProvider turns a decoded height grid into a
//     renderable regular-grid GltfModel with lifted heights, slope normals,
//     a tightened height range, and overlay UVs.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/cache/HttpCache.h"
#include "earth_engine/providers/ImageTileBodyCheck.h"
#include "earth_engine/content/HeightmapTerrainContentProvider.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/debug/Contracts.h"
#include "earth_engine/platform/bridge/PlatformBridge.h"
#include "earth_engine/providers/HeightmapTerrainProvider.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/threading/CancellationToken.h"
#include "earth_engine/tiling/LoadedTerrainHeightSampler.h"
#include "earth_engine/tiling/TileKey.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TilesetTile.h"

namespace earth_engine {
namespace {

// PlatformBridge double: get() responds 200 with a dummy body; decodeImage()
// synthesises a width×height RGB image whose pixels are the Mapbox Terrain-RGB
// (or Terrarium) encoding of a caller-supplied height field. Row-major,
// top(north)→bottom(south).
class SyntheticHeightBridge final : public PlatformBridge {
public:
    int width = 4;
    int height = 4;
    HeightmapTerrainProvider::Encoding encoding =
        HeightmapTerrainProvider::Encoding::MapboxTerrainRgb;
    std::function<float(int col, int row)> heightAt;

    void onEnterBackground() override {}
    void onEnterForeground() override {}

    // 网络响应可配置(默认 200 + PNG 魔数体:响应体魔数校验上线后,假体
    // 必须过白名单才能走到 decodeImage)。
    int httpStatus = 200;
    std::vector<uint8_t> httpBody{
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0};

    std::unique_ptr<HttpRequest> get(
        const std::string&,
        std::function<void(int, std::vector<uint8_t>)> callback,
        HttpRequestOptions = {}) override {
        if (callback) {
            callback(httpStatus, httpBody);
        }
        return nullptr;
    }

    std::string cacheDirectory() const override { return "/tmp/mock"; }
    std::string documentsDirectory() const override { return "/tmp/mock"; }

    std::unique_ptr<DecodedImage> decodeImage(const uint8_t*,
                                              size_t) override {
        auto image = std::make_unique<DecodedImage>();
        image->width = width;
        image->height = height;
        image->channels = 3;
        image->bytesPerChannel = 1;
        image->pixels.resize(static_cast<size_t>(width * height * 3));
        for (int row = 0; row < height; ++row) {
            for (int col = 0; col < width; ++col) {
                const float h = heightAt ? heightAt(col, row) : 0.0f;
                uint8_t r = 0, g = 0, b = 0;
                encode(h, r, g, b);
                const size_t off =
                    static_cast<size_t>((row * width + col) * 3);
                image->pixels[off] = r;
                image->pixels[off + 1] = g;
                image->pixels[off + 2] = b;
            }
        }
        return image;
    }

    void log(LogLevel, const std::string&, const std::string&) override {}
    DeviceInfo deviceInfo() const override { return DeviceInfo{}; }
    std::string getToken(const std::string&) const override { return ""; }

private:
    void encode(float h, uint8_t& r, uint8_t& g, uint8_t& b) const {
        if (encoding == HeightmapTerrainProvider::Encoding::MapboxTerrainRgb) {
            double e = std::round((static_cast<double>(h) + 10000.0) * 10.0);
            e = std::clamp(e, 0.0, 16777215.0);
            const uint32_t ei = static_cast<uint32_t>(e);
            r = static_cast<uint8_t>((ei >> 16) & 0xFF);
            g = static_cast<uint8_t>((ei >> 8) & 0xFF);
            b = static_cast<uint8_t>(ei & 0xFF);
        } else {
            // Terrarium: h = R*256 + G + B/256 - 32768
            double v = std::clamp(static_cast<double>(h) + 32768.0, 0.0,
                                  65535.99);
            r = static_cast<uint8_t>(static_cast<int>(v / 256.0) & 0xFF);
            const double rem = v - std::floor(v / 256.0) * 256.0;
            g = static_cast<uint8_t>(static_cast<int>(rem) & 0xFF);
            b = static_cast<uint8_t>(
                static_cast<int>((rem - std::floor(rem)) * 256.0) & 0xFF);
        }
    }
};


// requestTileContent 的回调契约是异步的(生产网络路径与 HttpCache 命中路径均
// 在 worker 线程完成,见 HeightmapTerrainProvider::requestTile),先等回调落地
// 再断言;超时上限 2s 防死等。
void waitForContentCallback(const std::atomic<bool>& called) {
    for (int i = 0; i < 2000 && !called.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ---- P0: decode contract ----------------------------------------------------

TEST(HeightmapTerrainDecode, MapboxTerrainRgbRoundTrip) {
    SyntheticHeightBridge bridge;
    bridge.width = 5;
    bridge.height = 5;
    bridge.encoding = HeightmapTerrainProvider::Encoding::MapboxTerrainRgb;
    // Known field: rises 100 m/col east, 10 m/row south. Includes a negative.
    bridge.heightAt = [](int col, int row) {
        return -50.0f + 100.0f * static_cast<float>(col) +
               10.0f * static_cast<float>(row);
    };

    HeightmapTerrainProvider provider("file:///{z}/{x}/{y}.png", "");
    provider.setEncoding(HeightmapTerrainProvider::Encoding::MapboxTerrainRgb);
    provider.setPlatformBridge(&bridge);

    const uint8_t dummy = 0;
    std::unique_ptr<DecodedHeightmap> hm = provider.decodeTile(&dummy, 1);
    ASSERT_NE(hm, nullptr);
    ASSERT_TRUE(hm->valid());
    EXPECT_EQ(hm->tileSize, 5);

    for (int row = 0; row < 5; ++row) {
        for (int col = 0; col < 5; ++col) {
            const float expected =
                -50.0f + 100.0f * static_cast<float>(col) +
                10.0f * static_cast<float>(row);
            const float actual =
                hm->heights[static_cast<size_t>(row * 5 + col)];
            // 0.1 m encode step → <0.05 m rounding error.
            EXPECT_NEAR(actual, expected, 0.06f)
                << "col=" << col << " row=" << row;
        }
    }
    EXPECT_NEAR(hm->minHeight, -50.0f, 0.06f);
    EXPECT_NEAR(hm->maxHeight, -50.0f + 400.0f + 40.0f, 0.06f);
}

// ---- P1: content provider builds a regular-grid mesh ------------------------

TEST(HeightmapTerrainContent, BuildsRegularGridMeshWithLiftedHeights) {
    SyntheticHeightBridge bridge;
    bridge.width = 5;   // → gridSize 4 → 5×5 = 25 vertices
    bridge.height = 5;
    bridge.encoding = HeightmapTerrainProvider::Encoding::MapboxTerrainRgb;
    // East-rising ramp so slope normals must tilt off the pure geodetic normal.
    // Use a deep-zoom tile (small ground cells) so the slope is steep enough to
    // read: at z12 a cell is ~2 km wide, so 500 m/col ≈ a ~14° slope.
    bridge.heightAt = [](int col, int /*row*/) {
        return 500.0f * static_cast<float>(col);  // 0..2000 m
    };

    auto provider = std::make_unique<HeightmapTerrainProvider>(
        "file:///{z}/{x}/{y}.png", "");
    provider->setEncoding(
        HeightmapTerrainProvider::Encoding::MapboxTerrainRgb);
    provider->setZoomRange(0, 12);
    provider->setPlatformBridge(&bridge);

    HeightmapTerrainContentProvider content(std::move(provider), 12);

    const TileKey key{"XYZ-WebMercator", 12, 3400, 1500};
    ASSERT_TRUE(content.supportsTile(key));

    CancellationToken token;
    std::atomic<bool> called{false};
    TileContentLoadResult captured;
    content.requestTileContent(
        key,
        token,
        [&](const TileKey&, TileContentLoadResult result) {
            captured = std::move(result);
            called = true;
        });

    waitForContentCallback(called);
    ASSERT_TRUE(called);
    EXPECT_EQ(captured.status, TileLoadStatus::Renderable);
    EXPECT_TRUE(captured.terrainRenderContent);
    ASSERT_NE(captured.gltfModel, nullptr);
    ASSERT_FALSE(captured.gltfModel->primitives.empty());

    const GltfPrimitive& primitive = captured.gltfModel->primitives.front();
    // The real-surface (no-skirt) part is exactly the 5×5 grid; skirt geometry
    // is appended after it and tracked via skirtMetadata.
    ASSERT_TRUE(primitive.skirtMetadata.has_value());
    EXPECT_EQ(primitive.skirtMetadata->noSkirtVerticesCount, 25u);       // 5×5
    EXPECT_EQ(primitive.skirtMetadata->noSkirtIndicesCount, 4u * 4u * 6u);
    // 4 edges × 5 vertices = 20 skirt vertices (corners duplicated per edge);
    // 4 edges × 4 segments × 2 tris × 3 = 96 skirt indices.
    EXPECT_EQ(primitive.vertices.size(), 25u + 20u);
    EXPECT_EQ(primitive.indices.size(), 4u * 4u * 6u + 96u);

    // Height range tightened to the real min/max (heightFactor 1.0).
    ASSERT_TRUE(captured.metadata.terrainHeightRange.has_value());
    EXPECT_NEAR(captured.metadata.terrainHeightRange->first, 0.0, 1.0);
    EXPECT_NEAR(captured.metadata.terrainHeightRange->second, 2000.0, 1.0);

    // Overlay UVs computed (draping works identically to real terrain).
    ASSERT_FALSE(captured.gltfModel->rasterOverlayDetails.empty());

    // Grid (no-skirt) vertices lifted off the ellipsoid with slope normals.
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    double maxHeightAbove = 0.0;
    bool anyTiltedNormal = false;
    const uint32_t gridCount = primitive.skirtMetadata->noSkirtVerticesCount;
    for (uint32_t i = 0; i < gridCount; ++i) {
        const SurfaceVertex& v = primitive.vertices[i];
        const std::optional<Cartographic> c =
            ellipsoid.tryCartesianToCartographic(v.positionEcef);
        if (c) {
            maxHeightAbove = std::max(maxHeightAbove, c->height());
        }
        // Slope normals: on an east-rising ramp, normals tilt off the local
        // geodetic (radial) normal → dot < ~0.9999 for at least one vertex, but
        // still generally outward (dot > 0).
        const Vec3 geodetic = ellipsoid.geodeticSurfaceNormal(v.positionEcef);
        const double d = v.normalEcef.dot(geodetic);
        EXPECT_GT(d, 0.0);  // outward-facing
        if (d < 0.999) {
            anyTiltedNormal = true;
        }
    }
    EXPECT_GT(maxHeightAbove, 1800.0);  // ~2000 m ramp reached
    EXPECT_TRUE(anyTiltedNormal);       // slope shading, not flat geodetic

    // Skirt vertices hang below their edge parents (dropped along the normal),
    // so at least one skirt vertex sits well under the lowest grid height.
    double minSkirtHeight = 1e9;
    for (size_t i = gridCount; i < primitive.vertices.size(); ++i) {
        const std::optional<Cartographic> c =
            ellipsoid.tryCartesianToCartographic(primitive.vertices[i].positionEcef);
        if (c) {
            minSkirtHeight = std::min(minSkirtHeight, c->height());
        }
    }
    EXPECT_LT(minSkirtHeight, 0.0);  // dropped below the 0 m ramp base
}

// ---- P2: distance-continuous geomorph delta baked in the worker ------------

TEST(HeightmapTerrainContent, BakesGeomorphDeltaFromCoarseSelfDownsample) {
    SyntheticHeightBridge bridge;
    bridge.width = 5;   // → gridSize 4 (even) → 5×5 = 25 vertices, even subgrid
    bridge.height = 5;  //   {0,2,4} spans both edges → morph delta well-defined
    bridge.encoding = HeightmapTerrainProvider::Encoding::MapboxTerrainRgb;
    // Column-only quadratic h = 100·col² (row-independent so latitude-projection
    // non-linearity can't perturb it; column sampling round-trips exactly through
    // web-mercator). The 2× coarse-self downsample (osgEarth neighbour-average)
    // then has an exact analytic delta = coarse − true:
    //   even col → coarse == self            → delta 0
    //   odd  col → ½(h[col-1] + h[col+1])−h  = ½·200 = +100 m  (curvature of x²)
    bridge.heightAt = [](int col, int /*row*/) {
        return 100.0f * static_cast<float>(col) * static_cast<float>(col);
    };

    auto provider = std::make_unique<HeightmapTerrainProvider>(
        "file:///{z}/{x}/{y}.png", "");
    provider->setEncoding(
        HeightmapTerrainProvider::Encoding::MapboxTerrainRgb);
    provider->setZoomRange(0, 12);
    provider->setPlatformBridge(&bridge);

    HeightmapTerrainContentProvider content(std::move(provider), 12);

    const TileKey key{"XYZ-WebMercator", 12, 3400, 1500};
    CancellationToken token;
    std::atomic<bool> called{false};
    TileContentLoadResult captured;
    content.requestTileContent(
        key,
        token,
        [&](const TileKey&, TileContentLoadResult result) {
            captured = std::move(result);
            called = true;
        });

    waitForContentCallback(called);
    ASSERT_TRUE(called);
    ASSERT_NE(captured.gltfModel, nullptr);
    ASSERT_FALSE(captured.gltfModel->primitives.empty());
    const GltfPrimitive& primitive = captured.gltfModel->primitives.front();
    ASSERT_TRUE(primitive.skirtMetadata.has_value());
    const uint32_t gridCount = primitive.skirtMetadata->noSkirtVerticesCount;
    ASSERT_EQ(gridCount, 25u);  // 5×5 grid (skirt vertices follow)

    constexpr int kN = 5;  // vertices per side (gridSize 4 + 1)
    bool anyOddColMorph = false;
    for (uint32_t i = 0; i < gridCount; ++i) {  // grid part only
        const int col = static_cast<int>(i) % kN;
        const float delta = primitive.vertices[i].geomorphHeightDelta;
        if (col % 2 == 0) {
            // Even columns are coarse grid points: morph delta is exactly 0
            // (coarse == self), regardless of sampling noise.
            EXPECT_NEAR(delta, 0.0f, 1e-2f)
                << "even col=" << col << " index=" << i;
        } else {
            // Odd columns morph from the coarse (parent-like) surface: the
            // quadratic curvature gives a known +100 m delta.
            EXPECT_NEAR(delta, 100.0f, 1.0f)
                << "odd col=" << col << " index=" << i;
            anyOddColMorph = true;
        }
    }
    EXPECT_TRUE(anyOddColMorph);  // morph machinery actually produced deltas
}

// A flat height field morphs to nothing — every vertex is already its own
// coarse-self, so the baked geomorph delta must be identically 0 (no morph, no
// spurious vertical wobble during LOD transitions over flat terrain).
TEST(HeightmapTerrainContent, FlatFieldBakesZeroGeomorphDelta) {
    SyntheticHeightBridge bridge;
    bridge.width = 5;
    bridge.height = 5;
    bridge.encoding = HeightmapTerrainProvider::Encoding::MapboxTerrainRgb;
    bridge.heightAt = [](int, int) { return 750.0f; };  // constant plateau

    auto provider = std::make_unique<HeightmapTerrainProvider>(
        "file:///{z}/{x}/{y}.png", "");
    provider->setEncoding(
        HeightmapTerrainProvider::Encoding::MapboxTerrainRgb);
    provider->setZoomRange(0, 12);
    provider->setPlatformBridge(&bridge);

    HeightmapTerrainContentProvider content(std::move(provider), 12);
    const TileKey key{"XYZ-WebMercator", 12, 3400, 1500};
    CancellationToken token;
    std::atomic<bool> called{false};
    TileContentLoadResult captured;
    content.requestTileContent(
        key,
        token,
        [&](const TileKey&, TileContentLoadResult result) {
            captured = std::move(result);
            called = true;
        });

    waitForContentCallback(called);
    ASSERT_TRUE(called);
    ASSERT_NE(captured.gltfModel, nullptr);
    ASSERT_FALSE(captured.gltfModel->primitives.empty());
    for (const SurfaceVertex& v :
         captured.gltfModel->primitives.front().vertices) {
        EXPECT_NEAR(v.geomorphHeightDelta, 0.0f, 1e-2f);
    }
}

// ---- P5: terrain height sampler (camera clamp / picking / vector draping) ---
// The sampler is source-agnostic — it walks GltfModel triangles and only checks
// isTerrainRenderContent(). A CPU-baked regular-grid heightmap tile produces a
// real per-vertex terrain GltfModel exactly like QM, so LoadedTerrainHeightSampler
// must return the baked terrain height (NOT sea-level 0, NOT a skirt-dropped
// value) for a point over the tile. This locks the "reuse, no code change"
// claim for the height-query path.
TEST(HeightmapTerrainContent, HeightSamplerReadsBakedTerrainHeight) {
    SyntheticHeightBridge bridge;
    bridge.width = 5;
    bridge.height = 5;
    bridge.encoding = HeightmapTerrainProvider::Encoding::MapboxTerrainRgb;
    // Flat plateau so the expected sample height is unambiguous regardless of
    // which surface triangle the query ray hits.
    bridge.heightAt = [](int, int) { return 640.0f; };

    auto provider = std::make_unique<HeightmapTerrainProvider>(
        "file:///{z}/{x}/{y}.png", "");
    provider->setEncoding(
        HeightmapTerrainProvider::Encoding::MapboxTerrainRgb);
    provider->setZoomRange(0, 12);
    provider->setPlatformBridge(&bridge);

    HeightmapTerrainContentProvider content(std::move(provider), 12);
    const TileKey key{"XYZ-WebMercator", 12, 3400, 1500};
    CancellationToken token;
    std::atomic<bool> called{false};
    TileContentLoadResult captured;
    content.requestTileContent(
        key, token,
        [&](const TileKey&, TileContentLoadResult result) {
            captured = std::move(result);
            called = true;
        });
    waitForContentCallback(called);
    ASSERT_TRUE(called);
    ASSERT_NE(captured.gltfModel, nullptr);

    // Register the baked tile in a loaded-terrain registry exactly as the
    // runtime does (identity transform — heightmap vertices are absolute ECEF).
    const Rectangle bounds =
        TileScheme::createXYZWebMercator()->tileToRectangle(key);
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto tile = std::make_unique<TilesetTile>(key, bounds);
    tile->content.renderContent.prepareGltfContent(
        std::move(captured.gltfModel), Mat4::identity());
    tile->content.renderContent.setTerrainRenderContent(true);
    // The runtime retains the decoded heightmap alongside the baked mesh; the
    // height sampler reads it (the mesh is a flat shared template under GPU
    // displacement, so the heightmap is the source of truth).
    tile->content.renderContent.setRetainedHeightmap(
        std::move(captured.retainedHeightmap));
    tile->markRenderContentDone();
    tiles.emplace("heightmap/12/3400/1500", std::move(tile));

    // Query the tile centre: must resolve to the ~640 m plateau, not sea level.
    const double lon = (bounds.west() + bounds.east()) * 0.5;
    const double lat = (bounds.south() + bounds.north()) * 0.5;
    const std::optional<float> sampled =
        LoadedTerrainHeightSampler::sampleHeightOptional(tiles, lon, lat);
    ASSERT_TRUE(sampled.has_value());
    EXPECT_NEAR(*sampled, 640.0f, 5.0f);

    // A point outside the tile has no covering terrain → nullopt (must not be
    // reported as a real 0 m sea-level height).
    EXPECT_FALSE(LoadedTerrainHeightSampler::sampleHeightOptional(
                     tiles, bounds.east() + 0.05, lat)
                     .has_value());
}

// ---- 响应体魔数校验(HTTP 200 + CDN NoSuchKey XML 硬化) ---------------------

// 实测取证的 CDN 错误体形态:HTTP 200 + OSS NoSuchKey XML(边缘过期负缓存)。
std::vector<uint8_t> noSuchKeyXmlBody() {
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Error><Code>NoSuchKey</Code>"
        "<Message>The specified key does not exist.</Message></Error>";
    return std::vector<uint8_t>(xml.begin(), xml.end());
}

TEST(ImageTileBodyCheck, WhitelistsImageMagicsAndRejectsErrorBodies) {
    const std::vector<uint8_t> png{
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0};
    const std::vector<uint8_t> jpeg{
        0xFF, 0xD8, 0xFF, 0xE0, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::vector<uint8_t> webp{
        'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
    EXPECT_TRUE(looksLikeImageTileBody(png));
    EXPECT_TRUE(looksLikeImageTileBody(jpeg));
    EXPECT_TRUE(looksLikeImageTileBody(webp));

    EXPECT_FALSE(looksLikeImageTileBody(noSuchKeyXmlBody()));
    EXPECT_FALSE(looksLikeImageTileBody(std::vector<uint8_t>{}));
    // 12 字节下限:比最短魔数还短的体一律非法。
    EXPECT_FALSE(looksLikeImageTileBody(
        std::vector<uint8_t>{0x89, 'P', 'N', 'G'}));
}

// 200 + XML 错误体 → 不入 HttpCache,状态为 RetryLater(瞬时,非永久 Failed)。
TEST(HeightmapTerrainFetch, XmlErrorBodyIsRetryLaterAndNotCached) {
    SyntheticHeightBridge bridge;
    bridge.httpBody = noSuchKeyXmlBody();

    HeightmapTerrainProvider provider(
        "http://cdn.test/magic-a/{z}/{x}/{y}.png", "");
    provider.setEncoding(HeightmapTerrainProvider::Encoding::MapboxTerrainRgb);
    provider.setPlatformBridge(&bridge);

    const TileKey key{"XYZ-WebMercator", 5, 11, 13};
    const std::string url = provider.buildUrl(key);
    HttpCache::shared().remove(url);

    std::atomic<bool> called{false};
    TileLoadStatus status = TileLoadStatus::Failed;
    provider.requestTile(
        key,
        CancellationToken{},
        [&](const TileKey&, TerrainTileLoadResult result) {
            status = result.status;
            called = true;
        });
    waitForContentCallback(called);
    ASSERT_TRUE(called);
    EXPECT_EQ(status, TileLoadStatus::RetryLater);
    EXPECT_FALSE(HttpCache::shared().contains(url));
}

// 缓存里的历史坏体(校验上线前毒入的 XML)自愈:命中即清除,当次转网络
// 重取并成功解码,缓存回填为合法图像体。
TEST(HeightmapTerrainFetch, PoisonedCacheEntryIsEvictedAndRefetched) {
    SyntheticHeightBridge bridge;
    bridge.heightAt = [](int, int) { return 120.0f; };

    HeightmapTerrainProvider provider(
        "http://cdn.test/magic-b/{z}/{x}/{y}.png", "");
    provider.setEncoding(HeightmapTerrainProvider::Encoding::MapboxTerrainRgb);
    provider.setPlatformBridge(&bridge);

    const TileKey key{"XYZ-WebMercator", 5, 21, 9};
    const std::string url = provider.buildUrl(key);
    HttpCache::shared().put(url, noSuchKeyXmlBody());
    ASSERT_TRUE(HttpCache::shared().contains(url));

    std::atomic<bool> called{false};
    TileLoadStatus status = TileLoadStatus::Failed;
    bool gotHeightmap = false;
    provider.requestTile(
        key,
        CancellationToken{},
        [&](const TileKey&, TerrainTileLoadResult result) {
            status = result.status;
            gotHeightmap = result.heightmap != nullptr;
            called = true;
        });
    waitForContentCallback(called);
    ASSERT_TRUE(called);
    EXPECT_EQ(status, TileLoadStatus::Renderable);
    EXPECT_TRUE(gotHeightmap);
    // 坏体已被替换为网络重取的合法体。
    EXPECT_EQ(HttpCache::shared().get(url), bridge.httpBody);
    HttpCache::shared().remove(url);
}

// ---- 契约 DemNodataSentinel 的一对控制组 -----------------------------------
//
// 一条永远不会触发的契约,和一条每次都通过的契约,在日志里长得一模一样。所以
// 正例(不响)与反例(响)都要有,否则契约本身成了新的不可观测物。
//
// 反例的构造方式同时把头文件里记的那个理论假阳性钉死:Terrarium 编码不注册隐式
// 哨兵,若源里真出现 -10000m,契约就会报警。这既证明通路是活的,也说明这条警告
// 在 Terrarium 源上要先核对编码再动手。

// 契约计数是进程级单调量,断言一律比增量,与测试顺序无关。
uint32_t noDataViolations() {
    return contracts::totalViolations(contracts::Id::DemNodataSentinel);
}

TileContentLoadResult buildTileWith(
    SyntheticHeightBridge& bridge,
    HeightmapTerrainProvider::Encoding encoding) {
    bridge.encoding = encoding;
    auto provider = std::make_unique<HeightmapTerrainProvider>(
        "file:///{z}/{x}/{y}.png", "");
    provider->setEncoding(encoding);
    provider->setZoomRange(0, 12);
    provider->setPlatformBridge(&bridge);
    HeightmapTerrainContentProvider content(std::move(provider), 12);

    std::atomic<bool> called{false};
    TileContentLoadResult captured;
    content.requestTileContent(
        TileKey{"XYZ-WebMercator", 12, 3400, 1500},
        CancellationToken{},
        [&](const TileKey&, TileContentLoadResult result) {
            captured = std::move(result);
            called = true;
        });
    waitForContentCallback(called);
    EXPECT_TRUE(called);
    return captured;
}

TEST(ContractDemNodataSentinel, RegisteredSentinelDoesNotViolate) {
    // Terrain-RGB:解码器隐式注册 -10000 哨兵,底值样本被排除在 minHeight 之外。
    SyntheticHeightBridge bridge;
    bridge.width = 5;
    bridge.height = 5;
    // 一半是真实高程、一半是 nodata 底值 —— 正是"数据空洞"的形态。
    bridge.heightAt = [](int col, int /*row*/) {
        return col < 2 ? -10000.0f : 100.0f * static_cast<float>(col);
    };
    const uint32_t before = noDataViolations();
    buildTileWith(bridge, HeightmapTerrainProvider::Encoding::MapboxTerrainRgb);
    EXPECT_EQ(0u, noDataViolations() - before);
}

TEST(ContractDemNodataSentinel, UnregisteredSentinelViolates) {
    // Terrarium:不注册隐式哨兵。同样的 -10000 样本会被当合法高度统计进
    // minHeight —— 契约必须响。
    SyntheticHeightBridge bridge;
    bridge.width = 5;
    bridge.height = 5;
    bridge.heightAt = [](int col, int /*row*/) {
        return col < 2 ? -10000.0f : 100.0f * static_cast<float>(col);
    };
    const uint32_t before = noDataViolations();
    buildTileWith(bridge, HeightmapTerrainProvider::Encoding::Terrarium);
    EXPECT_EQ(1u, noDataViolations() - before);
}

TEST(ContractDemNodataSentinel, CleanDataDoesNotViolate) {
    // 完全没有 nodata 的源:两种编码都不该报警(排除"这条契约见谁咬谁")。
    SyntheticHeightBridge bridge;
    bridge.width = 5;
    bridge.height = 5;
    bridge.heightAt = [](int col, int /*row*/) {
        return 100.0f * static_cast<float>(col);
    };
    const uint32_t before = noDataViolations();
    buildTileWith(bridge, HeightmapTerrainProvider::Encoding::MapboxTerrainRgb);
    buildTileWith(bridge, HeightmapTerrainProvider::Encoding::Terrarium);
    EXPECT_EQ(0u, noDataViolations() - before);
}

}  // namespace
}  // namespace earth_engine
