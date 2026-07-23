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
#include "earth_engine/content/HeightmapTerrainContentProvider.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/math/Mat4.h"
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

    void onMemoryPressure() override {}
    void onEnterBackground() override {}
    void onEnterForeground() override {}

    std::unique_ptr<HttpRequest> get(
        const std::string&,
        std::function<void(int, std::vector<uint8_t>)> callback,
        HttpRequestOptions = {}) override {
        if (callback) {
            callback(200, std::vector<uint8_t>{0u});
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

}  // namespace
}  // namespace earth_engine
