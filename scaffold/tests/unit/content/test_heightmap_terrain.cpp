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

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/content/HeightmapTerrainContentProvider.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/platform/bridge/PlatformBridge.h"
#include "earth_engine/providers/HeightmapTerrainProvider.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/threading/CancellationToken.h"
#include "earth_engine/tiling/TileKey.h"

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
    bool called = false;
    TileContentLoadResult captured;
    content.requestTileContent(
        key,
        token,
        [&](const TileKey&, TileContentLoadResult result) {
            captured = std::move(result);
            called = true;
        });

    ASSERT_TRUE(called);
    EXPECT_EQ(captured.status, TileLoadStatus::Renderable);
    EXPECT_TRUE(captured.terrainRenderContent);
    ASSERT_NE(captured.gltfModel, nullptr);
    ASSERT_FALSE(captured.gltfModel->primitives.empty());

    const GltfPrimitive& primitive = captured.gltfModel->primitives.front();
    EXPECT_EQ(primitive.vertices.size(), 25u);            // 5×5 grid
    EXPECT_EQ(primitive.indices.size(), 4u * 4u * 6u);    // 4×4 cells × 2 tris

    // Height range tightened to the real min/max (heightFactor 1.0).
    ASSERT_TRUE(captured.metadata.terrainHeightRange.has_value());
    EXPECT_NEAR(captured.metadata.terrainHeightRange->first, 0.0, 1.0);
    EXPECT_NEAR(captured.metadata.terrainHeightRange->second, 2000.0, 1.0);

    // Overlay UVs computed (draping works identically to real terrain).
    ASSERT_FALSE(captured.gltfModel->rasterOverlayDetails.empty());

    // Vertices lifted off the ellipsoid: at least one sits well above height 0.
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    double maxHeightAbove = 0.0;
    bool anyTiltedNormal = false;
    for (const SurfaceVertex& v : primitive.vertices) {
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
}

}  // namespace
}  // namespace earth_engine
