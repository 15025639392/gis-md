#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/QuantizedMeshTerrainProvider.h"
#include "earth_engine/providers/RasterOverlayTileProvider.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Transforms.h"
#include "earth_engine/core/math/OrientedBoundingBox.h"
#include "earth_engine/core/math/Plane.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/scene/Scene.h"
#include "earth_engine/terrain/QuantizedMeshParser.h"
#include "earth_engine/tiling/TilePlan.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TileSurface.h"
#include "earth_engine/tiling/Tileset.h"

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

using namespace earth_engine;

namespace earth_engine {
struct TilesetTestAccess {
    static TilesetTile* ensureTile(Tileset& tileset, const TileKey& key) {
        return tileset.ensureTile(key);
    }

    static std::string terrainCacheKey(Tileset& tileset, const TileKey& key) {
        return tileset.terrainCacheKey(key);
    }

    static void putTerrainCache(
        Tileset& tileset,
        const TileKey& key,
        std::unique_ptr<DecodedHeightmap> heightmap) {
        tileset.terrainCache_[terrainCacheKey(tileset, key)] =
            std::move(heightmap);
    }

    static void ensureTileChildren(Tileset& tileset, TilesetTile& tile) {
        tileset.ensureTileChildren(tile);
    }

    static bool canRefine(Tileset& tileset, const TilesetTile& tile) {
        return tileset.canRefine(tile);
    }

    static bool isTileRenderable(Tileset& tileset, const TilesetTile& tile) {
        return tileset.isTileRenderable(tile);
    }

    static void renderSelectedTile(Tileset& tileset, TilesetTile& tile) {
        tileset.renderSelectedTile(tile, 1.0, true);
    }

    static bool culledTileReportsMissingRenderableForForbidHoles(
        Tileset& tileset,
        const TilesetTile& tile) {
        auto details = tileset.createTraversalDetailsForCulledTile(tile);
        return !details.allAreRenderable &&
               !details.anyWereRenderedLastFrame &&
               details.notYetRenderableCount == 1;
    }

    static bool culledTileIsIgnoredWithoutForbidHoles(
        Tileset& tileset,
        const TilesetTile& tile) {
        auto details = tileset.createTraversalDetailsForCulledTile(tile);
        return details.allAreRenderable &&
               !details.anyWereRenderedLastFrame &&
               details.notYetRenderableCount == 0;
    }

    static bool singleTileDetailsAnyWereRenderedLastFrame(
        Tileset& tileset,
        const TilesetTile& tile) {
        return tileset.createTraversalDetailsForSingleTile(tile)
            .anyWereRenderedLastFrame;
    }

    static void requestMissingTile(Tileset& tileset, const TileKey& key) {
        tileset.requestMissingTiles({
            Tileset::TileLoadRequest{
                key,
                Tileset::TileLoadPriorityGroup::Normal}});
    }

    static void requestMissingTilesWithPriorities(
        Tileset& tileset,
        const TileKey& firstKey,
        double firstPriority,
        const TileKey& secondKey,
        double secondPriority) {
        tileset.requestMissingTiles({
            Tileset::TileLoadRequest{
                firstKey,
                Tileset::TileLoadPriorityGroup::Normal,
                firstPriority},
            Tileset::TileLoadRequest{
                secondKey,
                Tileset::TileLoadPriorityGroup::Normal,
                secondPriority}});
    }

    static void requestMissingPreload(Tileset& tileset, const TileKey& key) {
        tileset.requestMissingTiles({
            Tileset::TileLoadRequest{
                key,
                Tileset::TileLoadPriorityGroup::Preload}});
    }

    static void requestMissingUrgent(Tileset& tileset, const TileKey& key) {
        tileset.requestMissingTiles({
            Tileset::TileLoadRequest{
                key,
                Tileset::TileLoadPriorityGroup::Urgent}});
    }

    static void processPendingUploads(Tileset& tileset) {
        tileset.processPendingUploads();
    }

    static bool loadQueueEmpty(const Tileset& tileset) {
        return tileset.loadQueue_.empty();
    }

    static void queuePreload(Tileset& tileset, const TileKey& key) {
        tileset.queueTileLoad(key, Tileset::TileLoadPriorityGroup::Preload);
    }

    static void queueNormal(Tileset& tileset, const TileKey& key) {
        tileset.queueTileLoad(key, Tileset::TileLoadPriorityGroup::Normal);
    }

    static void queueUrgent(Tileset& tileset, const TileKey& key) {
        tileset.queueTileLoad(key, Tileset::TileLoadPriorityGroup::Urgent);
    }

    static int64_t maximumCachedBytes() {
        return Tileset::kMaximumCachedBytes;
    }

    static int64_t estimateTileBytes(const TilesetTile& tile) {
        return Tileset::estimateTileBytes(tile);
    }

    static void updateTotalBytesUsed(Tileset& tileset) {
        tileset.updateTotalBytesUsed();
    }

    static void markEligibleForUnloading(Tileset& tileset, const TileKey& key) {
        tileset.markEligibleForUnloading(terrainCacheKey(tileset, key));
    }

    static void unloadCachedBytes(Tileset& tileset, int64_t maximumCachedBytes) {
        tileset.unloadCachedBytes(maximumCachedBytes, nullptr);
    }

    static void clearChildrenRecursively(Tileset& tileset, TilesetTile& tile) {
        tileset.clearChildrenRecursively(&tile, nullptr);
    }

    static TilesetTile* findTile(Tileset& tileset, const TileKey& key) {
        auto it = tileset.tiles_.find(terrainCacheKey(tileset, key));
        if (it == tileset.tiles_.end()) return nullptr;
        return it->second.get();
    }

    static void ensureTileMesh(Tileset& tileset, TilesetTile& tile) {
        tileset.ensureTileMesh(tile);
    }

    static void unloadTileContent(Tileset& tileset,
                                  TilesetTile& tile,
                                  IPrepareRendererResources* pPrepRenderer) {
        tileset.unloadTileContent(tile, pPrepRenderer);
    }

    static void buildTileDrawCommand(Tileset& tileset,
                                     Renderer& renderer,
                                     TilesetTile& tile,
                                     RenderCommandList& commands,
                                     float transitionOpacity) {
        tileset.buildTileDrawCommand(
            renderer,
            tile,
            commands,
            transitionOpacity);
    }

    static const Vec3& localOrigin(const TilesetTile& tile) {
        return tile.localOrigin;
    }

    static Vec3 tileBoundsCenter(const Rectangle& bounds) {
        return Tileset::tileBoundsCenter(bounds);
    }

    static double tileBoundsRadius(const TilesetTile& tile,
                                   const Vec3& center) {
        return Tileset::tileBoundsRadius(tile, center);
    }

    static std::optional<OrientedBoundingBox> tileBoundingRegionObb(
        const TilesetTile& tile) {
        return Tileset::tileBoundingRegionObb(tile);
    }

    static bool tileIntersectsFrustum(const TilesetTile& tile,
                                      const Frustum& frustum) {
        return Tileset::tileIntersectsFrustum(tile, frustum);
    }

    static double approximateDistanceToTileBounds(
        const TilesetTile& tile,
        const Vec3& cameraPosition) {
        return Tileset::approximateDistanceToTileBounds(
            tile,
            cameraPosition);
    }

    static void selectTiles(Tileset& tileset, const FrameState& frameState) {
        tileset.selectTiles(frameState);
    }

    static void beginTilePlan(Tileset& tileset) {
        tileset.tilePlan_ = TilePlan{};
    }

    static const TilePlan& tilePlan(Tileset& tileset) {
        return tileset.tilePlan_;
    }

    static void updateLodTransitions(Tileset& tileset,
                                     double deltaSeconds) {
        tileset.updateLodTransitions(deltaSeconds);
    }

    static size_t fadingOutSetSize(const Tileset& tileset) {
        return tileset.tilesFadingOut_.size();
    }

    static void setLastCamera(Tileset& tileset,
                              const Vec3& position,
                              const Vec3& direction) {
        tileset.lastCameraPosition_ = position;
        tileset.lastCameraDirection_ = direction;
    }

    static void setOcclusionState(
        Tileset& tileset,
        TileOcclusionState state) {
        tileset.setOcclusionCallback(
            [state](const TilesetTile&) { return state; });
    }

    static void setOcclusionCallback(
        Tileset& tileset,
        Tileset::OcclusionCallback callback) {
        tileset.setOcclusionCallback(std::move(callback));
    }

    static TileOcclusionState checkOcclusion(
        Tileset& tileset,
        const TilesetTile& tile) {
        return tileset.checkOcclusion(tile);
    }
};
} // namespace earth_engine

namespace {

int gFailures = 0;

void check(bool condition, const std::string& name) {
    if (condition) {
        std::cout << "  OK  " << name << "\n";
    } else {
        std::cout << "FAIL  " << name << "\n";
        ++gFailures;
    }
}

FrameState::SelectorView makeSelectorView(const Camera& camera,
                                          int viewportWidth,
                                          int viewportHeight) {
    FrameState::SelectorView view;
    view.position = camera.position();
    view.direction = camera.direction();
    view.frustum = camera.frustum(
        static_cast<double>(viewportWidth),
        static_cast<double>(viewportHeight));
    view.verticalFovRadians = camera.verticalFovRadians();
    view.viewportHeightPixels = viewportHeight;
    return view;
}

template <typename T>
void appendPod(std::vector<uint8_t>& bytes, T value) {
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(T));
}

void pad4(std::vector<uint8_t>& bytes, uint8_t pad) {
    while ((bytes.size() % 4u) != 0u) {
        bytes.push_back(pad);
    }
}

uint16_t zigZagEncode16(int32_t value) {
    return static_cast<uint16_t>(
        value >= 0 ? value * 2 : (-value * 2) - 1);
}

std::vector<uint8_t> makeQuantizedMeshBytes(
    const std::string& metadataJson = "",
    bool includeSkirtEdges = false,
    bool includeOctNormals = false,
    const Vec3& boundingSphereCenterEcef = Vec3::zero(),
    float minimumHeight = 0.0f,
    float maximumHeight = 100.0f,
    const Vec3& tileCenterEcef = Vec3::zero()) {
    std::vector<uint8_t> bytes;

    appendPod<double>(bytes, tileCenterEcef.x());
    appendPod<double>(bytes, tileCenterEcef.y());
    appendPod<double>(bytes, tileCenterEcef.z());
    appendPod<float>(bytes, minimumHeight);
    appendPod<float>(bytes, maximumHeight);
    appendPod<double>(bytes, boundingSphereCenterEcef.x());
    appendPod<double>(bytes, boundingSphereCenterEcef.y());
    appendPod<double>(bytes, boundingSphereCenterEcef.z());
    appendPod<double>(bytes, 0.0);
    for (int i = 0; i < 3; ++i) appendPod<double>(bytes, 0.0);
    appendPod<uint32_t>(bytes, 3);

    const uint16_t u[] = {
        zigZagEncode16(0),
        zigZagEncode16(32767),
        zigZagEncode16(-32767)
    };
    const uint16_t v[] = {
        zigZagEncode16(0),
        zigZagEncode16(0),
        zigZagEncode16(32767)
    };
    const uint16_t h[] = {
        zigZagEncode16(0),
        zigZagEncode16(0),
        zigZagEncode16(0)
    };
    for (uint16_t value : u) appendPod<uint16_t>(bytes, value);
    for (uint16_t value : v) appendPod<uint16_t>(bytes, value);
    for (uint16_t value : h) appendPod<uint16_t>(bytes, value);

    appendPod<uint32_t>(bytes, 1);
    appendPod<uint16_t>(bytes, 0);
    appendPod<uint16_t>(bytes, 0);
    appendPod<uint16_t>(bytes, 0);
    auto appendEdge = [&](std::initializer_list<uint16_t> indices) {
        appendPod<uint32_t>(bytes, static_cast<uint32_t>(indices.size()));
        for (uint16_t index : indices) appendPod<uint16_t>(bytes, index);
    };
    if (includeSkirtEdges) {
        appendEdge({0, 2});
        appendEdge({1, 0});
        appendEdge({1, 2});
        appendEdge({2, 1});
    } else {
        for (int i = 0; i < 4; ++i) appendPod<uint32_t>(bytes, 0);
    }

    if (includeOctNormals) {
        appendPod<uint8_t>(bytes, 1);
        appendPod<uint32_t>(bytes, 6);
        const uint8_t normals[] = {
            128, 128,
            255, 128,
            128, 255
        };
        bytes.insert(bytes.end(), normals, normals + sizeof(normals));
    }

    if (!metadataJson.empty()) {
        appendPod<uint8_t>(bytes, 4);
        appendPod<uint32_t>(
            bytes,
            static_cast<uint32_t>(sizeof(uint32_t) + metadataJson.size()));
        appendPod<uint32_t>(bytes, static_cast<uint32_t>(metadataJson.size()));
        bytes.insert(bytes.end(), metadataJson.begin(), metadataJson.end());
    }
    return bytes;
}

void writeBytes(const std::filesystem::path& path,
                const std::vector<uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

std::unique_ptr<DecodedHeightmap> makeFlatHeightmap(float heightMeters);

RasterOverlay::Options makeRasterOverlayOptions() {
    RasterOverlay::Options options{};
    options.maximumSimultaneousTileLoads = 20;
    options.maximumScreenSpaceError = 2.0;
    options.minimumZoom = 0;
    options.maximumZoom = 0;
    options.visible = true;
    options.opacity = 1.0f;
    return options;
}

class DummyBuffer final : public Buffer {
public:
    explicit DummyBuffer(size_t byteSize) : byteSize_(byteSize) {}
    DummyBuffer(size_t byteSize, const void* data) : byteSize_(byteSize) {
        if (data && byteSize > 0) {
            const auto* begin = static_cast<const uint8_t*>(data);
            bytes_.assign(begin, begin + byteSize);
        }
    }
    size_t size() const override { return byteSize_; }
    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    size_t byteSize_ = 0;
    std::vector<uint8_t> bytes_;
};

class DummyShaderProgram final : public ShaderProgram {};

class DummyTexture final : public Texture {
public:
    DummyTexture(int width, int height) : width_(width), height_(height) {}
    int width() const override { return width_; }
    int height() const override { return height_; }

private:
    int width_ = 0;
    int height_ = 0;
};

class DummyRenderDevice final : public RenderDevice {
public:
    Backend backendType() const override { return Backend::OpenGLES; }
    int maxTextureSize() const override { return 4096; }
    int maxDrawBuffers() const override { return 4; }
    bool supportsFloatTextures() const override { return true; }
    bool supportsInstancing() const override { return true; }
    std::string rendererString() const override { return "DummyRenderDevice"; }

    std::unique_ptr<Texture> createTexture(const TextureDesc& desc) override {
        if (!allowTextureCreation) return nullptr;
        lastTextureDesc = desc;
        ++createdTextureCount;
        return std::make_unique<DummyTexture>(desc.width, desc.height);
    }

    bool updateTextureRegion(Texture*,
                             int,
                             int,
                             int,
                             int,
                             const uint8_t*,
                             size_t) override {
        return false;
    }

    std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) override {
        return std::make_unique<DummyBuffer>(desc.size, desc.data);
    }

    bool updateBuffer(Buffer*, size_t, const void*, size_t) override {
        return false;
    }

    std::unique_ptr<ShaderProgram> createShader(const ShaderDesc&) override {
        return std::make_unique<DummyShaderProgram>();
    }

    std::unique_ptr<Framebuffer> createFramebuffer(
        const FramebufferDesc&) override {
        return nullptr;
    }

    void beginFrame() override {}
    void submit(const RenderCommandList& commands) override {
        submittedCommands = commands;
    }
    void endFrame() override {}
    void onSurfaceCreated() override {}
    void onSurfaceChanged(int, int) override {}
    void onSurfaceDestroyed() override {}

    RenderCommandList submittedCommands;
    TextureDesc lastTextureDesc;
    int createdTextureCount = 0;
    bool allowTextureCreation = false;
};

std::unique_ptr<GltfModel> makeTriangleGltfModel() {
    auto model = std::make_unique<GltfModel>();
    GltfPrimitive primitive;
    primitive.vertices.resize(3);
    primitive.vertices[0].positionEcef = Vec3(0.0, 0.0, 0.0);
    primitive.vertices[1].positionEcef = Vec3(1.0, 0.0, 0.0);
    primitive.vertices[2].positionEcef = Vec3(0.0, 1.0, 0.0);
    primitive.vertices[0].normalEcef = Vec3::unitZ();
    primitive.vertices[1].normalEcef = Vec3::unitZ();
    primitive.vertices[2].normalEcef = Vec3::unitZ();
    primitive.vertices[0].uv = {0.0f, 0.0f};
    primitive.vertices[1].uv = {1.0f, 0.0f};
    primitive.vertices[2].uv = {0.0f, 1.0f};
    primitive.vertexTexCoords[0] = {
        std::array<float, 2>{0.0f, 0.0f},
        std::array<float, 2>{1.0f, 0.0f},
        std::array<float, 2>{0.0f, 1.0f}};
    primitive.vertexTexCoords[1] = {
        std::array<float, 2>{0.25f, 0.75f},
        std::array<float, 2>{0.5f, 0.75f},
        std::array<float, 2>{0.25f, 0.5f}};
    primitive.vertexColors = {
        std::array<float, 4>{0.25f, 0.5f, 0.75f, 1.0f},
        std::array<float, 4>{1.0f, 0.0f, 0.5f, 0.5f},
        std::array<float, 4>{0.0f, 1.0f, 0.25f, 0.25f}};
    primitive.vertexTangents = {
        std::array<float, 4>{1.0f, 0.0f, 0.0f, 1.0f},
        std::array<float, 4>{1.0f, 0.0f, 0.0f, 1.0f},
        std::array<float, 4>{1.0f, 0.0f, 0.0f, 1.0f}};
    primitive.indices = {0, 1, 2};
    model->primitives.push_back(std::move(primitive));
    return model;
}

std::unique_ptr<GltfModel> makeTexturedTriangleGltfModel() {
    auto model = makeTriangleGltfModel();
    GltfTexture texture;
    texture.image.width = 1;
    texture.image.height = 1;
    texture.image.channels = 4;
    texture.image.pixels = {255, 64, 32, 128};
    texture.sampler.mipmap = false;
    texture.sampler.minFilter = GltfTextureFilter::Nearest;
    texture.sampler.magFilter = GltfTextureFilter::Nearest;
    texture.sampler.wrapS = GltfTextureWrap::MirroredRepeat;
    texture.sampler.wrapT = GltfTextureWrap::ClampToEdge;
    model->textures.push_back(std::move(texture));
    model->primitives[0].baseColorTextureIndex = 0u;
    GltfTextureBinding baseColorBinding;
    baseColorBinding.textureIndex = 0u;
    baseColorBinding.texCoord = 1;
    baseColorBinding.transform.offset = {0.25f, 0.5f};
    baseColorBinding.transform.scale = {2.0f, 3.0f};
    baseColorBinding.transform.rotation = 1.57079632679f;
    model->primitives[0].baseColorTexture = baseColorBinding;
    model->primitives[0].alphaMode = GltfAlphaMode::Blend;
    return model;
}

std::vector<uint8_t> makeTriangleGlbBytes() {
    auto appendU32 = [](std::vector<uint8_t>& bytes, uint32_t value) {
        bytes.push_back(static_cast<uint8_t>(value & 0xffu));
        bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
        bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
        bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
    };
    auto appendU16 = [](std::vector<uint8_t>& bytes, uint16_t value) {
        bytes.push_back(static_cast<uint8_t>(value & 0xffu));
        bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    };
    auto appendF32 = [](std::vector<uint8_t>& bytes, float value) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof(float));
    };
    auto pad4 = [](std::vector<uint8_t>& bytes, uint8_t pad) {
        while ((bytes.size() % 4u) != 0u) {
            bytes.push_back(pad);
        }
    };

    std::vector<uint8_t> bin;
    const size_t positionsOffset = bin.size();
    appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 1.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 0.0f); appendF32(bin, 1.0f); appendF32(bin, 0.0f);

    const size_t normalsOffset = bin.size();
    for (int i = 0; i < 3; ++i) {
        appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 1.0f);
    }

    const size_t uvOffset = bin.size();
    appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 1.0f); appendF32(bin, 0.0f);
    appendF32(bin, 0.0f); appendF32(bin, 1.0f);

    const size_t indicesOffset = bin.size();
    appendU16(bin, 0); appendU16(bin, 1); appendU16(bin, 2);
    pad4(bin, 0);

    const std::string jsonText =
        std::string("{") +
        "\"asset\":{\"version\":\"2.0\"}," +
        "\"scene\":0," +
        "\"scenes\":[{\"nodes\":[0]}]," +
        "\"nodes\":[{\"mesh\":0}]," +
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"mode\":4}]}]," +
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]," +
        "\"bufferViews\":[" +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(positionsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(normalsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(uvOffset) + ",\"byteLength\":24}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(indicesOffset) + ",\"byteLength\":6}" +
        "]," +
        "\"accessors\":[" +
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}," +
        "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}" +
        "]}";

    std::vector<uint8_t> jsonBytes(jsonText.begin(), jsonText.end());
    pad4(jsonBytes, 0x20);

    std::vector<uint8_t> glb;
    appendU32(glb, 0x46546C67u);
    appendU32(glb, 2u);
    appendU32(glb, static_cast<uint32_t>(
        12 + 8 + jsonBytes.size() + 8 + bin.size()));
    appendU32(glb, static_cast<uint32_t>(jsonBytes.size()));
    appendU32(glb, 0x4E4F534Au);
    glb.insert(glb.end(), jsonBytes.begin(), jsonBytes.end());
    appendU32(glb, static_cast<uint32_t>(bin.size()));
    appendU32(glb, 0x004E4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

std::vector<uint8_t> makeNativeGpuInstancedTriangleGlbBytes() {
    std::vector<uint8_t> bin;
    const size_t positionsOffset = bin.size();
    appendPod<float>(bin, 0.0f); appendPod<float>(bin, 0.0f); appendPod<float>(bin, 0.0f);
    appendPod<float>(bin, 1.0f); appendPod<float>(bin, 0.0f); appendPod<float>(bin, 0.0f);
    appendPod<float>(bin, 0.0f); appendPod<float>(bin, 1.0f); appendPod<float>(bin, 0.0f);

    const size_t normalsOffset = bin.size();
    for (int i = 0; i < 3; ++i) {
        appendPod<float>(bin, 0.0f); appendPod<float>(bin, 0.0f); appendPod<float>(bin, 1.0f);
    }

    const size_t uvOffset = bin.size();
    appendPod<float>(bin, 0.0f); appendPod<float>(bin, 0.0f);
    appendPod<float>(bin, 1.0f); appendPod<float>(bin, 0.0f);
    appendPod<float>(bin, 0.0f); appendPod<float>(bin, 1.0f);

    const size_t indicesOffset = bin.size();
    appendPod<uint16_t>(bin, 0); appendPod<uint16_t>(bin, 1); appendPod<uint16_t>(bin, 2);
    pad4(bin, 0);

    const size_t translationsOffset = bin.size();
    appendPod<float>(bin, 0.0f); appendPod<float>(bin, 0.0f); appendPod<float>(bin, 5.0f);
    appendPod<float>(bin, 0.0f); appendPod<float>(bin, 0.0f); appendPod<float>(bin, 25.0f);
    pad4(bin, 0);

    const std::string jsonText =
        std::string("{") +
        "\"asset\":{\"version\":\"2.0\"}," +
        "\"extensionsUsed\":[\"EXT_mesh_gpu_instancing\"]," +
        "\"extensionsRequired\":[\"EXT_mesh_gpu_instancing\"]," +
        "\"scene\":0," +
        "\"scenes\":[{\"nodes\":[0]}]," +
        "\"nodes\":[{\"mesh\":0,\"extensions\":{\"EXT_mesh_gpu_instancing\":{"
        "\"attributes\":{\"TRANSLATION\":4}}}}]," +
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"mode\":4}]}]," +
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]," +
        "\"bufferViews\":[" +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(positionsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(normalsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(uvOffset) + ",\"byteLength\":24}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(indicesOffset) + ",\"byteLength\":6}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(translationsOffset) + ",\"byteLength\":24}" +
        "]," +
        "\"accessors\":[" +
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}," +
        "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}," +
        "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}" +
        "]}";

    std::vector<uint8_t> jsonBytes(jsonText.begin(), jsonText.end());
    pad4(jsonBytes, 0x20);

    std::vector<uint8_t> glb;
    appendPod<uint32_t>(glb, 0x46546C67u);
    appendPod<uint32_t>(glb, 2u);
    appendPod<uint32_t>(
        glb,
        static_cast<uint32_t>(
            12 + 8 + jsonBytes.size() + 8 + bin.size()));
    appendPod<uint32_t>(glb, static_cast<uint32_t>(jsonBytes.size()));
    appendPod<uint32_t>(glb, 0x4E4F534Au);
    glb.insert(glb.end(), jsonBytes.begin(), jsonBytes.end());
    appendPod<uint32_t>(glb, static_cast<uint32_t>(bin.size()));
    appendPod<uint32_t>(glb, 0x004E4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

std::vector<uint8_t> makeI3dmBytes(std::vector<uint8_t> gltfPayload,
                                   const std::vector<Vec3>& positions) {
    std::vector<uint8_t> featureBinary;
    const size_t positionsOffset = featureBinary.size();
    for (const Vec3& position : positions) {
        appendPod<float>(featureBinary, static_cast<float>(position.x()));
        appendPod<float>(featureBinary, static_cast<float>(position.y()));
        appendPod<float>(featureBinary, static_cast<float>(position.z()));
    }
    pad4(featureBinary, 0);

    const std::string featureJson =
        std::string("{") +
        "\"INSTANCES_LENGTH\":" + std::to_string(positions.size()) + "," +
        "\"POSITION\":{\"byteOffset\":" +
        std::to_string(positionsOffset) + "}" +
        "}";
    std::vector<uint8_t> featureBytes(
        featureJson.begin(),
        featureJson.end());
    pad4(featureBytes, 0x20);

    std::vector<uint8_t> i3dm;
    i3dm.push_back('i');
    i3dm.push_back('3');
    i3dm.push_back('d');
    i3dm.push_back('m');
    appendPod<uint32_t>(i3dm, 1u);
    appendPod<uint32_t>(
        i3dm,
        static_cast<uint32_t>(
            32 + featureBytes.size() +
            featureBinary.size() +
            gltfPayload.size()));
    appendPod<uint32_t>(i3dm, static_cast<uint32_t>(featureBytes.size()));
    appendPod<uint32_t>(i3dm, static_cast<uint32_t>(featureBinary.size()));
    appendPod<uint32_t>(i3dm, 0u);
    appendPod<uint32_t>(i3dm, 0u);
    appendPod<uint32_t>(i3dm, 1u);
    i3dm.insert(i3dm.end(), featureBytes.begin(), featureBytes.end());
    i3dm.insert(i3dm.end(), featureBinary.begin(), featureBinary.end());
    i3dm.insert(i3dm.end(), gltfPayload.begin(), gltfPayload.end());
    return i3dm;
}

struct ExternalGltfBytes {
    std::vector<uint8_t> jsonBytes;
    std::vector<uint8_t> binBytes;
};

ExternalGltfBytes makeTriangleExternalGltfBytes() {
    std::vector<uint8_t> bin;
    const size_t positionsOffset = bin.size();
    appendPod<float>(bin, 0.0f); appendPod<float>(bin, 0.0f); appendPod<float>(bin, 0.0f);
    appendPod<float>(bin, 1.0f); appendPod<float>(bin, 0.0f); appendPod<float>(bin, 0.0f);
    appendPod<float>(bin, 0.0f); appendPod<float>(bin, 1.0f); appendPod<float>(bin, 0.0f);

    const size_t normalsOffset = bin.size();
    for (int i = 0; i < 3; ++i) {
        appendPod<float>(bin, 0.0f); appendPod<float>(bin, 0.0f); appendPod<float>(bin, 1.0f);
    }

    const size_t uvOffset = bin.size();
    appendPod<float>(bin, 0.0f); appendPod<float>(bin, 0.0f);
    appendPod<float>(bin, 1.0f); appendPod<float>(bin, 0.0f);
    appendPod<float>(bin, 0.0f); appendPod<float>(bin, 1.0f);

    const size_t indicesOffset = bin.size();
    appendPod<uint16_t>(bin, 0); appendPod<uint16_t>(bin, 1); appendPod<uint16_t>(bin, 2);
    while ((bin.size() % 4u) != 0u) bin.push_back(0);

    const std::string jsonText =
        std::string("{") +
        "\"asset\":{\"version\":\"2.0\"}," +
        "\"scene\":0," +
        "\"scenes\":[{\"nodes\":[0]}]," +
        "\"nodes\":[{\"mesh\":0}]," +
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"mode\":4}]}]," +
        "\"buffers\":[{\"uri\":\"triangle.bin\",\"byteLength\":" +
        std::to_string(bin.size()) + "}]," +
        "\"bufferViews\":[" +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(positionsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(normalsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(uvOffset) + ",\"byteLength\":24}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(indicesOffset) + ",\"byteLength\":6}" +
        "]," +
        "\"accessors\":[" +
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}," +
        "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}" +
        "]}";
    return ExternalGltfBytes{
        std::vector<uint8_t>(jsonText.begin(), jsonText.end()),
        std::move(bin)};
}

void testRasterOverlayProviderRetention() {
    DebugImageryProvider imagery;
    auto scheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    TileKey key{scheme->id(), 3, 4, 2};

    provider.setFrameNumber(1);
    RasterOverlayTile* first = provider.getTile(key);
    check(first != nullptr, "RasterOverlayTileProvider: getTile creates tile");
    check(provider.getCachedTileCount() == 1,
          "RasterOverlayTileProvider: cache has one tile after getTile");

    provider.setFrameNumber(2);
    provider.trimUnusedTiles();
    check(provider.getCachedTileCount() == 1,
          "RasterOverlayTileProvider: tile survives one unused frame");

    provider.setFrameNumber(122);
    provider.markUsed(key);
    provider.trimUnusedTiles();
    check(provider.getCachedTileCount() == 1,
          "RasterOverlayTileProvider: markUsed(TileKey) refreshes retention");

    provider.setFrameNumber(243);
    provider.trimUnusedTiles();
    check(provider.getCachedTileCount() == 0,
          "RasterOverlayTileProvider: stale tile is eventually trimmed");
}

void testActivatedRasterOverlayBindsProviderOwner() {
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        makeRasterOverlayOptions());

    auto provider = std::make_unique<RasterOverlayTileProvider>(
        overlay->getProvider(),
        overlay->getTileScheme(),
        nullptr);
    RasterOverlayTileProvider* rawProvider = provider.get();

    ActivatedRasterOverlay activated(*overlay);
    activated.setTileProvider(std::move(provider));

    check(rawProvider->getOwner() == overlay.get(),
          "ActivatedRasterOverlay: setTileProvider binds provider owner like cesium-native");
}

void testTileSelectionKickedStatePreservesOriginalResult() {
    TileSelectionState rendered = TileSelectionState::Rendered;
    kickSelectionState(rendered);
    check(rendered == TileSelectionState::RenderedAndKicked,
          "TileSelectionState: rendered kick state is preserved");
    check(selectionWasKicked(rendered),
          "TileSelectionState: rendered kick is detected");
    check(originalSelectionState(rendered) == TileSelectionState::Rendered,
          "TileSelectionState: rendered original result survives kick");

    TileSelectionState refined = TileSelectionState::Refined;
    kickSelectionState(refined);
    check(refined == TileSelectionState::RefinedAndKicked,
          "TileSelectionState: refined kick state is preserved");
    check(selectionWasKicked(refined),
          "TileSelectionState: refined kick is detected");
    check(originalSelectionState(refined) == TileSelectionState::Refined,
          "TileSelectionState: refined original result survives kick");

    TileSelectionState culled = TileSelectionState::Culled;
    kickSelectionState(culled);
    check(culled == TileSelectionState::Culled,
          "TileSelectionState: culled state is not kicked");
    check(!selectionWasKicked(culled),
          "TileSelectionState: culled state is not treated as kicked");
}

void testEllipsoidScaleToGeodeticSurfaceMatchesCesiumNative() {
    const auto& ellipsoid = Ellipsoid::WGS84();

    check(std::abs(ellipsoid.semiMinorAxis() - 6356752.3142451793) < 1e-9,
          "Ellipsoid: WGS84 polar radius matches cesium-native");

    check(!ellipsoid.tryScaleToGeodeticSurface(Vec3::zero()).has_value(),
          "Ellipsoid: scaleToGeodeticSurface returns empty at center like cesium-native");
    check(!ellipsoid.tryCartesianToCartographic(Vec3::zero()).has_value(),
          "Ellipsoid: cartesianToCartographic returns empty at center like cesium-native");

    const auto nearCenterSurface =
        ellipsoid.tryScaleToGeodeticSurface(Vec3(1.0, 0.0, 0.0));
    check(nearCenterSurface.has_value() &&
              std::abs(nearCenterSurface->x() - ellipsoid.semiMajorAxis()) < 1e-6 &&
              std::abs(nearCenterSurface->y()) < 1e-12 &&
              std::abs(nearCenterSurface->z()) < 1e-12,
          "Ellipsoid: nonzero near-center projection uses cesium-native radial intersection");

    const auto surfaceCart = ellipsoid.tryCartesianToCartographic(
        Vec3(ellipsoid.semiMajorAxis() + 1000.0, 0.0, 0.0));
    check(surfaceCart.has_value() &&
              std::abs(surfaceCart->longitude()) < 1e-12 &&
              std::abs(surfaceCart->latitude()) < 1e-12 &&
              std::abs(surfaceCart->height() - 1000.0) < 1e-6,
          "Ellipsoid: optional cartesianToCartographic preserves normal height");
}

void testEllipsoidRayIntersectionIntervalMatchesCesiumNative() {
    const Ellipsoid unitSphere(1.0, 1.0);

    const auto outside =
        unitSphere.rayIntersectionInterval(Vec3(2.0, 0.0, 0.0),
                                           Vec3(-1.0, 0.0, 0.0));
    check(outside.has_value() &&
              std::abs(outside->entryDistance - 1.0) < 1e-12 &&
              std::abs(outside->exitDistance - 3.0) < 1e-12,
          "Ellipsoid: ray outside interval matches cesium-native");

    const auto inside =
        Ellipsoid::WGS84().rayIntersectionInterval(
            Vec3(20000.0, 0.0, 0.0),
            Vec3(1.0, 0.0, 0.0));
    check(inside.has_value() &&
              inside->entryDistance == 0.0 &&
              std::abs(inside->exitDistance -
                       (Ellipsoid::WGS84().semiMajorAxis() - 20000.0)) < 1e-6,
          "Ellipsoid: ray inside interval starts at zero like cesium-native");

    check(!unitSphere
               .rayIntersectionInterval(Vec3(1.0, 0.0, 0.0),
                                        Vec3(0.0, 0.0, 1.0))
               .has_value(),
          "Ellipsoid: tangent ray on surface is a miss like cesium-native");

    check(!Ellipsoid(0.0, 0.0)
               .rayIntersectionInterval(Vec3(2.0, 0.0, 0.0),
                                        Vec3(-1.0, 0.0, 0.0))
               .has_value(),
          "Ellipsoid: degenerate radii return no ray interval like cesium-native");

    const auto entryPoint =
        unitSphere.rayIntersection(Vec3(0.5, 0.0, 0.0),
                                   Vec3(1.0, 0.0, 0.0));
    check(entryPoint.has_value() &&
              entryPoint->distanceTo(Vec3(0.5, 0.0, 0.0)) < 1e-12,
          "Ellipsoid: single ray intersection returns interval entry point");
}

void testTransformsEastNorthUpToFixedFrameMatchesCesiumNative() {
    auto columnMatches = [](const Mat4& m,
                            int col,
                            const Vec3& expected,
                            double epsilon = 1e-12) {
        return std::abs(m(0, col) - expected.x()) < epsilon &&
               std::abs(m(1, col) - expected.y()) < epsilon &&
               std::abs(m(2, col) - expected.z()) < epsilon;
    };

    const Mat4 zeroFrame =
        Transforms::eastNorthUpToFixedFrame(Vec3::zero());
    check(columnMatches(zeroFrame, 0, Vec3(0.0, 1.0, 0.0)) &&
              columnMatches(zeroFrame, 1, Vec3(-1.0, 0.0, 0.0)) &&
              columnMatches(zeroFrame, 2, Vec3(0.0, 0.0, 1.0)) &&
              columnMatches(zeroFrame, 3, Vec3::zero()),
          "Transforms: ENU fixed frame at origin matches cesium-native");

    const double polarRadius = Ellipsoid::WGS84().semiMinorAxis();
    const Mat4 northPoleFrame =
        Transforms::eastNorthUpToFixedFrame(Vec3(0.0, 0.0, polarRadius));
    check(columnMatches(northPoleFrame, 0, Vec3(0.0, 1.0, 0.0)) &&
              columnMatches(northPoleFrame, 1, Vec3(-1.0, 0.0, 0.0)) &&
              columnMatches(northPoleFrame, 2, Vec3(0.0, 0.0, 1.0)) &&
              columnMatches(northPoleFrame, 3, Vec3(0.0, 0.0, polarRadius)),
          "Transforms: ENU fixed frame at north pole matches cesium-native");

    const Mat4 southPoleFrame =
        Transforms::eastNorthUpToFixedFrame(Vec3(0.0, 0.0, -polarRadius));
    check(columnMatches(southPoleFrame, 0, Vec3(0.0, 1.0, 0.0)) &&
              columnMatches(southPoleFrame, 1, Vec3(1.0, 0.0, 0.0)) &&
              columnMatches(southPoleFrame, 2, Vec3(0.0, 0.0, -1.0)) &&
              columnMatches(southPoleFrame, 3, Vec3(0.0, 0.0, -polarRadius)),
          "Transforms: ENU fixed frame at south pole matches cesium-native");

    const double equatorialRadius = Ellipsoid::WGS84().semiMajorAxis();
    const Mat4 equatorFrame =
        Transforms::eastNorthUpToFixedFrame(Vec3(equatorialRadius, 0.0, 0.0));
    check(columnMatches(equatorFrame, 0, Vec3(0.0, 1.0, 0.0)) &&
              columnMatches(equatorFrame, 1, Vec3(0.0, 0.0, 1.0)) &&
              columnMatches(equatorFrame, 2, Vec3(1.0, 0.0, 0.0)) &&
              columnMatches(equatorFrame, 3, Vec3(equatorialRadius, 0.0, 0.0)),
          "Transforms: ENU fixed frame at equator matches cesium-native");
}

void testOrientedBoundingBoxDegenerateAxesMatchCesiumNative() {
    const Vec3 camera = Vec3::zero();

    OrientedBoundingBox allDegenerate(
        Vec3(1.0, 0.0, 0.0),
        Vec3::zero(),
        Vec3::zero(),
        Vec3::zero());
    check(allDegenerate.computeDistanceSquaredToPosition(camera) == 1.0,
          "OrientedBoundingBox: all degenerate axes distance matches cesium-native");

    OrientedBoundingBox oneAxis(
        Vec3(1.0, 0.0, 0.0),
        Vec3::unitX(),
        Vec3::zero(),
        Vec3::zero());
    check(oneAxis.computeDistanceSquaredToPosition(camera) == 0.0,
          "OrientedBoundingBox: one valid axis distance matches cesium-native");

    OrientedBoundingBox twoAxes(
        Vec3(1.0, 0.0, 0.0),
        Vec3::unitX(),
        Vec3::unitY(),
        Vec3::zero());
    check(twoAxes.computeDistanceSquaredToPosition(camera) == 0.0,
          "OrientedBoundingBox: two valid axes distance matches cesium-native");

    OrientedBoundingBox missingX(
        Vec3(1.0, 0.0, 0.0),
        Vec3::zero(),
        Vec3::unitY(),
        Vec3::unitZ());
    check(missingX.computeDistanceSquaredToPosition(camera) == 1.0,
          "OrientedBoundingBox: missing axis distance matches cesium-native");

    OrientedBoundingBox unitBox(
        Vec3::zero(),
        Vec3::unitX(),
        Vec3::unitY(),
        Vec3::unitZ());
    check(unitBox.intersectPlane(Plane(Vec3::unitX(), -1.0)) == -1,
          "OrientedBoundingBox: negative tangent plane is outside like cesium-native");
    check(unitBox.intersectPlane(Plane(Vec3::unitX(), 1.0)) == 1,
          "OrientedBoundingBox: positive tangent plane is inside like cesium-native");
}

void testBoundingSpherePlaneBoundaryMatchesCesiumNative() {
    BoundingSphere sphere(Vec3::zero(), 1.0);

    check(sphere.intersectPlane(Plane(Vec3::unitX(), -1.0)) == 0,
          "BoundingSphere: negative tangent plane intersects like cesium-native");
    check(sphere.intersectPlane(Plane(Vec3::unitX(), 1.0)) == 1,
          "BoundingSphere: positive tangent plane is inside like cesium-native");
}

void testRasterOverlayProviderRectangleTile() {
    DebugImageryProvider imagery;
    auto imageryScheme = TileScheme::createXYZWebMercator();
    auto geometryScheme = TileScheme::createGeographicTMS();
    RasterOverlayTileProvider provider(imagery, *imageryScheme, nullptr);

    TileKey geometryKey{"Geographic-TMS", 2, 4, 2};
    Rectangle geometryBounds = geometryScheme->tileToRectangle(geometryKey);

    provider.setFrameNumber(1);
    RasterOverlayTile* rectangleTile = provider.getTile(
        geometryBounds, 512.0, 512.0);
    check(rectangleTile != nullptr,
          "RasterOverlayTileProvider: rectangle tile is created");
    check(rectangleTile && rectangleTile->isRectangleTile(),
          "RasterOverlayTileProvider: rectangle tile uses native rectangle path");
    check(rectangleTile && rectangleTile->getRectangle() == geometryBounds,
          "RasterOverlayTileProvider: rectangle tile keeps geometry bounds");
    check(rectangleTile && !rectangleTile->getCacheKey().empty() &&
              rectangleTile->getCacheKey().find("rectangle/") == 0,
          "RasterOverlayTileProvider: rectangle tile uses rectangle cache key");
    check(rectangleTile && rectangleTile->getSourceZoom() == 3,
          "RasterOverlayTileProvider: source zoom follows target screen pixels");
    check(rectangleTile && rectangleTile->getTargetScreenPixelsX() == 512.0 &&
              rectangleTile->getTargetScreenPixelsY() == 512.0,
          "RasterOverlayTileProvider: target screen pixels are retained");

    provider.setFrameNumber(122);
    provider.markUsed(*rectangleTile);
    provider.trimUnusedTiles();
    check(provider.getCachedTileCount() == 1,
          "RasterOverlayTileProvider: markUsed(tile) retains rectangle tile");
}

void testRasterMappedUsesRenderContentDetailsRectangle() {
    DebugImageryProvider imagery;
    auto imageryScheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *imageryScheme, nullptr);
    provider.setFrameNumber(1);

    RasterOverlayDetails details;
    const Rectangle preciseRectangle =
        Rectangle::fromDegrees(-10.0, -5.0, 2.0, 7.0);
    details.setGeographicRectangle(preciseRectangle);

    RasterMappedToTilesetTile mapped;
    std::vector<RasterOverlayProjection> missingProjections;
    const TileKey geometryKey{"Geographic-TMS", 4, 8, 8};
    const RasterMappedToTilesetTile::MoreDetail moreDetail = mapped.update(
        geometryKey,
        details,
        512.0,
        512.0,
        provider,
        nullptr,
        missingProjections,
        nullptr,
        0);

    check(moreDetail == RasterMappedToTilesetTile::MoreDetail::Unknown,
          "RasterMappedToTilesetTile: unloaded details tile reports unknown detail");
    check(mapped.getLoadingTile() != nullptr &&
              mapped.getLoadingTile()->getRectangle() == preciseRectangle,
          "RasterMappedToTilesetTile: mapOverlayToTile uses render-content rectangle");
    check(missingProjections.empty(),
          "RasterMappedToTilesetTile: existing projection is not reported missing");
}

void testRasterMappedMissingProjectionUsesPlaceholder() {
    DebugImageryProvider imagery;
    auto imageryScheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *imageryScheme, nullptr);
    provider.setFrameNumber(1);

    RasterMappedToTilesetTile mapped;
    RasterOverlayDetails emptyDetails;
    std::vector<RasterOverlayProjection> missingProjections;
    const TileKey geometryKey{"Geographic-TMS", 4, 8, 8};
    const RasterMappedToTilesetTile::MoreDetail moreDetail = mapped.update(
        geometryKey,
        emptyDetails,
        512.0,
        512.0,
        provider,
        nullptr,
        missingProjections,
        nullptr,
        0);

    check(moreDetail == RasterMappedToTilesetTile::MoreDetail::No,
          "RasterMappedToTilesetTile: missing projection placeholder does not request detail");
    check(mapped.getLoadingTile() != nullptr &&
              mapped.getLoadingTile()->getState() ==
                  RasterOverlayTile::LoadState::Placeholder,
          "RasterMappedToTilesetTile: missing projection maps to placeholder");
    check(missingProjections.size() == 1 &&
              missingProjections.front() == RasterOverlayProjection::Geographic,
          "RasterMappedToTilesetTile: missing projection is recorded");
    check(provider.getCachedTileCount() == 0,
          "RasterMappedToTilesetTile: missing projection does not create synthetic tile");
}

void testRasterMappedAttachedUnknownReportsMoreDetail() {
    DebugImageryProvider imagery;
    auto imageryScheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider provider(imagery, *imageryScheme, nullptr);
    provider.setFrameNumber(1);

    RasterOverlayDetails details;
    details.setGeographicRectangle(
        Rectangle::fromDegrees(-20.0, -10.0, 0.0, 10.0));

    RasterMappedToTilesetTile mapped;
    std::vector<RasterOverlayProjection> missingProjections;
    const TileKey geometryKey{"Geographic-TMS", 3, 4, 2};

    const auto firstUpdate = mapped.update(
        geometryKey,
        details,
        512.0,
        512.0,
        provider,
        nullptr,
        missingProjections,
        nullptr,
        0);
    check(firstUpdate == RasterMappedToTilesetTile::MoreDetail::Unknown &&
              mapped.getLoadingTile() != nullptr,
          "RasterMappedToTilesetTile: unloaded tile reports unknown detail");

    RasterOverlayTile* loadingTile = mapped.getLoadingTile();
    loadingTile->setState(RasterOverlayTile::LoadState::Loaded);

    const auto promotedUpdate = mapped.update(
        geometryKey,
        details,
        512.0,
        512.0,
        provider,
        nullptr,
        missingProjections,
        nullptr,
        0);
    check(promotedUpdate == RasterMappedToTilesetTile::MoreDetail::Unknown &&
              mapped.getState() == RasterMappedToTilesetTile::State::Attached,
          "RasterMappedToTilesetTile: promoted Unknown availability is preserved");

    const auto attachedUpdate = mapped.update(
        geometryKey,
        details,
        512.0,
        512.0,
        provider,
        nullptr,
        missingProjections,
        nullptr,
        0);
    check(attachedUpdate == RasterMappedToTilesetTile::MoreDetail::Yes,
          "RasterMappedToTilesetTile: Attached fast path treats Unknown as more detail like cesium-native");
    check(!mapped.isMoreDetailAvailable(),
          "RasterMappedToTilesetTile: bool upsample query still requires explicit Yes");
}

void testRasterMappedFailureFallbackMatchesOverlayOwner() {
    auto overlayA = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        makeRasterOverlayOptions());
    auto overlayB = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        makeRasterOverlayOptions());

    RasterOverlayTileProvider providerA(
        overlayA->getProvider(),
        overlayA->getTileScheme(),
        nullptr);
    RasterOverlayTileProvider providerB(
        overlayB->getProvider(),
        overlayB->getTileScheme(),
        nullptr);
    providerA.setOwner(overlayA.get());
    providerB.setOwner(overlayB.get());
    providerA.setFrameNumber(1);
    providerB.setFrameNumber(1);

    RasterOverlayDetails parentDetails;
    parentDetails.setGeographicRectangle(
        Rectangle::fromDegrees(-20.0, -10.0, 0.0, 10.0));
    RasterOverlayDetails childDetails;
    childDetails.setGeographicRectangle(
        Rectangle::fromDegrees(-20.0, 0.0, -10.0, 10.0));

    TilesetTile parent(
        TileKey{"Geographic-TMS", 2, 2, 1},
        Rectangle::fromDegrees(-20.0, -10.0, 0.0, 10.0));
    TilesetTile child(
        TileKey{"Geographic-TMS", 3, 4, 2},
        Rectangle::fromDegrees(-20.0, 0.0, -10.0, 10.0),
        &parent);
    parent.rasterOverlays.resize(2);

    auto wrongOwner = std::make_unique<RasterMappedToTilesetTile>();
    std::vector<RasterOverlayProjection> wrongMissing;
    wrongOwner->update(
        parent.key,
        parentDetails,
        512.0,
        512.0,
        providerB,
        nullptr,
        wrongMissing,
        nullptr,
        0);
    wrongOwner->getLoadingTile()->setState(RasterOverlayTile::LoadState::Loaded);
    wrongOwner->getLoadingTile()->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::No);
    wrongOwner->update(
        parent.key,
        parentDetails,
        512.0,
        512.0,
        providerB,
        nullptr,
        wrongMissing,
        nullptr,
        0);
    RasterOverlayTile* wrongReady = wrongOwner->getReadyTile();
    parent.rasterOverlays[0] = std::move(wrongOwner);

    auto matchingOwner = std::make_unique<RasterMappedToTilesetTile>();
    std::vector<RasterOverlayProjection> matchingMissing;
    matchingOwner->update(
        parent.key,
        parentDetails,
        512.0,
        512.0,
        providerA,
        nullptr,
        matchingMissing,
        nullptr,
        1);
    matchingOwner->getLoadingTile()->setState(RasterOverlayTile::LoadState::Loaded);
    matchingOwner->getLoadingTile()->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::No);
    matchingOwner->update(
        parent.key,
        parentDetails,
        512.0,
        512.0,
        providerA,
        nullptr,
        matchingMissing,
        nullptr,
        1);
    RasterOverlayTile* matchingReady = matchingOwner->getReadyTile();
    parent.rasterOverlays[1] = std::move(matchingOwner);

    RasterMappedToTilesetTile childMapped;
    std::vector<RasterOverlayProjection> childMissing;
    childMapped.update(
        child.key,
        childDetails,
        512.0,
        512.0,
        providerA,
        nullptr,
        childMissing,
        nullptr,
        0);
    childMapped.getLoadingTile()->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::No);
    childMapped.getLoadingTile()->setState(RasterOverlayTile::LoadState::Failed);

    const auto fallbackUpdate = childMapped.update(
        child.key,
        childDetails,
        512.0,
        512.0,
        providerA,
        nullptr,
        childMissing,
        &parent,
        0);

    check(fallbackUpdate == RasterMappedToTilesetTile::MoreDetail::No,
          "RasterMappedToTilesetTile: original failed tile suppresses more-detail subdivision");
    check(childMapped.getReadyTile() == matchingReady &&
              childMapped.getReadyTile() != wrongReady,
          "RasterMappedToTilesetTile: failure fallback selects ancestor raster by owner, not slot index");
    check(childMapped.getLoadingTile() == nullptr,
          "RasterMappedToTilesetTile: owner-matched loaded ancestor is promoted to ready");
}

void testRasterOverlayNativeTranslationAndRendererWindow() {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey child{"Geographic-TMS", 2, 2, 0};
    const TileKey parent{"Geographic-TMS", 1, 1, 0};

    const TileTextureWindow nativeWindow =
        TileSurface::computeTranslationAndScale(
            scheme->tileToRectangle(child),
            scheme->tileToRectangle(parent));
    const TileTextureWindow rendererWindow =
        TileSurface::textureWindowForNorthWestUv(nativeWindow);

    check(std::abs(nativeWindow.offsetU - 0.0f) < 1e-6f &&
              std::abs(nativeWindow.offsetV - 0.0f) < 1e-6f &&
              std::abs(nativeWindow.scaleU - 0.5f) < 1e-6f &&
              std::abs(nativeWindow.scaleV - 0.5f) < 1e-6f,
          "RasterOverlayUtilities: native translation/scale uses minimumY origin");
    check(std::abs(rendererWindow.offsetU - 0.0f) < 1e-6f &&
              std::abs(rendererWindow.offsetV - 0.5f) < 1e-6f &&
              std::abs(rendererWindow.scaleU - 0.5f) < 1e-6f &&
              std::abs(rendererWindow.scaleV - 0.5f) < 1e-6f,
          "RasterOverlayUtilities: renderer window converts native V to north-west UV");
}

void testTilesetMaximumCachedBytesMatchesCesiumNativeDefault() {
    check(TilesetTestAccess::maximumCachedBytes() == 512LL * 1024 * 1024,
          "Tileset: maximumCachedBytes default matches cesium-native");
}

void testTilesetByteEstimateUsesActualMeshPayload() {
    TilesetTile tile;
    tile.mesh = std::make_unique<SurfaceTileMesh>();
    tile.mesh->vertices.resize(3);
    tile.mesh->indices.resize(6);
    tile.mesh->waterMask.data.resize(8);
    tile.mesh->metadataAvailability.resize(2);

    const int64_t expected =
        static_cast<int64_t>(tile.mesh->vertices.size() * sizeof(SurfaceVertex)) +
        static_cast<int64_t>(tile.mesh->indices.size() * sizeof(uint32_t)) +
        static_cast<int64_t>(tile.mesh->waterMask.data.size()) +
        static_cast<int64_t>(
            tile.mesh->metadataAvailability.size() * sizeof(std::array<int, 5>));

    check(TilesetTestAccess::estimateTileBytes(tile) == expected,
          "Tileset: byte estimate uses actual mesh payload");
}

void testQuantizedMeshAvailabilityLevels() {
    QuantizedMeshTerrainProvider provider("https://example.invalid/{z}/{x}/{y}.terrain");
    provider.setZoomRange(0, 10);

    provider.addAvailabilityRects(3, {{{0, 0, 0, 0}}});

    check(provider.supportsTile(TileKey{"Geographic-TMS", 0, 0, 0}),
          "QuantizedMeshTerrainProvider: availability keeps root requestable");
    check(provider.supportsTile(TileKey{"Geographic-TMS", 3, 0, 0}),
          "QuantizedMeshTerrainProvider: exact available level is supported");
    check(!provider.supportsTile(TileKey{"Geographic-TMS", 4, 0, 0}),
          "QuantizedMeshTerrainProvider: descendant is not implied without metadata");
}

void testQuantizedMeshAvailabilityInclusiveCenterBoundary() {
    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/{z}/{x}/{y}.terrain");
    provider.setZoomRange(0, 10);

    // cesium-native QuadtreeRectangleAvailability::isTileAvailable queries
    // the tile center against stored availability rectangles. Rectangle
    // boundaries are inclusive, and boundary positions check all child nodes.
    provider.addAvailabilityRects(2, {{{0, 0, 0, 0}}});

    check(provider.supportsTile(TileKey{"Geographic-TMS", 1, 0, 0}),
          "QuantizedMeshTerrainProvider: center on availability edge remains available");
    check(!provider.supportsTile(TileKey{"Geographic-TMS", 1, 1, 0}),
          "QuantizedMeshTerrainProvider: off-edge sibling is still unavailable");
}

void testQuantizedMeshMetadataAvailabilityStartsAtRoots() {
    QuantizedMeshTerrainProvider provider("https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 17,
      "metadataAvailability": 10
    })json";

    check(provider.configureFromLayerJson(layerJson, "https://example.invalid/layer.json"),
          "QuantizedMeshTerrainProvider: metadataAvailability layer.json configures");
    check(provider.supportsTile(TileKey{"Geographic-TMS", 0, 0, 0}) &&
              provider.supportsTile(TileKey{"Geographic-TMS", 0, 1, 0}),
          "QuantizedMeshTerrainProvider: metadataAvailability starts with roots available");
    check(provider.availabilityState(TileKey{"Geographic-TMS", 1, 0, 0}) ==
              TileAvailabilityState::Unknown,
          "QuantizedMeshTerrainProvider: unloaded metadata subtree is unknown");
    check(!provider.supportsTile(TileKey{"Geographic-TMS", 1, 0, 0}),
          "QuantizedMeshTerrainProvider: unknown metadata tile is not requestable");

    provider.markSubtreeLoaded(0, 0);
    check(provider.availabilityState(TileKey{"Geographic-TMS", 1, 0, 0}) ==
              TileAvailabilityState::NotAvailable,
          "QuantizedMeshTerrainProvider: loaded empty subtree becomes not available");

    QuantizedMeshTerrainProvider bounded("https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string boundedLayerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 10,
      "metadataAvailability": 10
    })json";
    check(bounded.configureFromLayerJson(
              boundedLayerJson, "https://example.invalid/layer.json"),
          "QuantizedMeshTerrainProvider: bounded subtree layer configures");
    check(bounded.isSubtreeLoaded(1, 0) &&
              bounded.isSubtreeLoaded(1, 1234),
          "QuantizedMeshTerrainProvider: subtrees past maxzoom are treated loaded");
    bounded.markSubtreeLoaded(1, 0);
    check(bounded.isSubtreeLoaded(1, 1234),
          "QuantizedMeshTerrainProvider: marking past-max subtree does not resize loaded set");

    QuantizedMeshTerrainProvider shadowed(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string shadowedLayerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 4,
      "metadataAvailability": 2,
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}],
        [{"startX":0,"startY":0,"endX":0,"endY":0}]
      ]
    })json";
    check(shadowed.configureFromLayerJson(
              shadowedLayerJson, "https://example.invalid/layer.json"),
          "QuantizedMeshTerrainProvider: metadataAvailability with available configures");
    check(shadowed.availabilityState(TileKey{"Geographic-TMS", 1, 0, 0}) ==
              TileAvailabilityState::Unknown,
          "QuantizedMeshTerrainProvider: metadataAvailability ignores layer available descendants");
    shadowed.markSubtreeLoaded(0, 0);
    check(shadowed.availabilityState(TileKey{"Geographic-TMS", 1, 0, 0}) ==
              TileAvailabilityState::NotAvailable,
          "QuantizedMeshTerrainProvider: ignored layer available stays unavailable after loaded parent subtree");
}

void testQuantizedMeshLayerJsonEmptyAvailabilityMatchesCesiumNative() {
    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 4
    })json";

    check(provider.configureFromLayerJson(
              layerJson, "https://example.invalid/layer.json"),
          "QuantizedMeshTerrainProvider: empty availability layer configures");
    check(provider.supportsTile(TileKey{"Geographic-TMS", 0, 0, 0}) &&
              provider.supportsTile(TileKey{"Geographic-TMS", 0, 1, 0}),
          "QuantizedMeshTerrainProvider: empty layer availability keeps roots requestable");
    check(provider.availabilityState(TileKey{"Geographic-TMS", 1, 0, 0}) ==
              TileAvailabilityState::NotAvailable,
          "QuantizedMeshTerrainProvider: empty layer availability does not imply descendants");
}

void testQuantizedMeshRejectsOutOfRangeGeographicTiles() {
    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 4
    })json";

    check(provider.configureFromLayerJson(
              layerJson, "https://example.invalid/layer.json"),
          "QuantizedMeshTerrainProvider: out-of-range layer configures");
    check(provider.supportsTile(TileKey{"Geographic-TMS", 0, 0, 0}) &&
              provider.supportsTile(TileKey{"Geographic-TMS", 0, 1, 0}),
          "QuantizedMeshTerrainProvider: valid Geographic-TMS roots remain available");
    check(!provider.supportsTile(TileKey{"Geographic-TMS", 0, -1, 0}) &&
              !provider.supportsTile(TileKey{"Geographic-TMS", 0, 2, 0}) &&
              !provider.supportsTile(TileKey{"Geographic-TMS", 0, 0, -1}) &&
              !provider.supportsTile(TileKey{"Geographic-TMS", 0, 0, 1}),
          "QuantizedMeshTerrainProvider: out-of-range roots are not available");

    QuantizedMeshTerrainProvider metadataProvider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string metadataLayerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 4,
      "metadataAvailability": 2
    })json";
    check(metadataProvider.configureFromLayerJson(
              metadataLayerJson, "https://example.invalid/layer.json"),
          "QuantizedMeshTerrainProvider: out-of-range metadata layer configures");
    check(metadataProvider.availabilityState(
              TileKey{"Geographic-TMS", 1, 4, 0}) ==
              TileAvailabilityState::NotAvailable &&
          metadataProvider.availabilityState(
              TileKey{"Geographic-TMS", 1, 0, 2}) ==
              TileAvailabilityState::NotAvailable,
          "QuantizedMeshTerrainProvider: out-of-range metadata tiles are not unknown");

    QuantizedMeshTerrainProvider legacyProvider(
        "https://example.invalid/{z}/{x}/{y}.terrain");
    legacyProvider.setZoomRange(0, 2);
    check(!legacyProvider.supportsTile(TileKey{"Geographic-TMS", 0, 2, 0}) &&
              !legacyProvider.supportsTile(TileKey{"Geographic-TMS", 1, 0, 2}),
          "QuantizedMeshTerrainProvider: legacy provider rejects out-of-range Geographic-TMS tiles");
}

void testQuantizedMeshLayerJsonMinzoomDoesNotGateAvailability() {
    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "minzoom": 5,
      "maxzoom": 8,
      "available": [
        [], [], [], [], [],
        [{"startX":16,"startY":16,"endX":16,"endY":16}]
      ]
    })json";

    check(provider.configureFromLayerJson(
              layerJson, "https://example.invalid/layer.json"),
          "QuantizedMeshTerrainProvider: minzoom layer configures");
    check(provider.minZoom() == 0,
          "QuantizedMeshTerrainProvider: layer.json minzoom does not become request gate");
    check(provider.supportsTile(TileKey{"Geographic-TMS", 0, 0, 0}),
          "QuantizedMeshTerrainProvider: availability below minzoom matches cesium-native");
}

void testQuantizedMeshLayerJsonMaxzoomDoesNotGateExplicitAvailability() {
    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 1,
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}],
        [],
        [{"startX":0,"startY":0,"endX":0,"endY":0}]
      ]
    })json";

    check(provider.configureFromLayerJson(
              layerJson, "https://example.invalid/layer.json"),
          "QuantizedMeshTerrainProvider: maxzoom layer configures");
    check(provider.maxZoom() == 1,
          "QuantizedMeshTerrainProvider: layer.json maxzoom is retained");
    check(provider.supportsTile(TileKey{"Geographic-TMS", 2, 0, 0}),
          "QuantizedMeshTerrainProvider: maxzoom does not gate explicit availability");
    check(!provider.supportsTile(TileKey{"Geographic-TMS", 3, 0, 0}),
          "QuantizedMeshTerrainProvider: explicit availability still does not imply descendants");
}

void testQuantizedMeshLayerJsonAvailabilityUint32Defaults() {
    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 2,
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}],
        [{"startX":0,"startY":0,"endX":-1,"endY":-1}]
      ]
    })json";

    check(provider.configureFromLayerJson(
              layerJson, "https://example.invalid/layer.json"),
          "QuantizedMeshTerrainProvider: uint32-default layer configures");
    check(provider.supportsTile(TileKey{"Geographic-TMS", 1, 0, 0}),
          "QuantizedMeshTerrainProvider: negative availability fields default to zero like cesium-native");
}

void testQuantizedMeshMetadataExtensionLengthPrefixMatchesCesiumNative() {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const std::string metadata = R"json({
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}],
        [{"startX":2,"startY":1,"endX":3,"endY":1}]
      ]
    })json";
    const std::vector<uint8_t> bytes = makeQuantizedMeshBytes(metadata);

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            scheme->tileToRectangle(rootKey));

    check(mesh != nullptr,
          "QuantizedMeshParser: cesium metadata extension parses");
    check(mesh && mesh->metadataAvailability.size() == 2,
          "QuantizedMeshParser: metadata availability skips length prefix");
    check(mesh && mesh->metadataAvailability[0] ==
              std::array<int, 5>{0, 0, 0, 1, 0} &&
              mesh->metadataAvailability[1] ==
              std::array<int, 5>{1, 2, 1, 3, 1},
          "QuantizedMeshParser: metadata availability level offsets match cesium-native");

    const std::vector<std::array<int, 5>> metadataOnly =
        QuantizedMeshParser::parseMetadataAvailability(bytes.data(), bytes.size());
    check(metadataOnly == mesh->metadataAvailability,
          "QuantizedMeshParser: metadata-only path matches cesium-native loadMetadata");

    const std::string negativeMetadata = R"json({
      "available": [
        [{"startX":-4,"startY":1,"endX":2,"endY":-8}]
      ]
    })json";
    const std::vector<uint8_t> negativeBytes =
        makeQuantizedMeshBytes(negativeMetadata);
    const std::vector<std::array<int, 5>> negativeOnly =
        QuantizedMeshParser::parseMetadataAvailability(
            negativeBytes.data(),
            negativeBytes.size());
    check(negativeOnly.size() == 1 &&
              negativeOnly[0] == std::array<int, 5>{0, 0, 1, 2, 0},
          "QuantizedMeshParser: metadata uint32 fields default like cesium-native");
}

void testQuantizedMeshSkirtNormalsCopyEdgeNormals() {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes("", true, true);

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            scheme->tileToRectangle(rootKey));

    check(mesh != nullptr,
          "QuantizedMeshParser: skirt-normal test mesh parses");
    const uint32_t firstSkirtVertex =
        mesh ? mesh->skirtMeta.noSkirtVerticesBegin +
                   mesh->skirtMeta.noSkirtVerticesCount
             : 0;
    check(mesh && firstSkirtVertex < mesh->vertices.size(),
          "QuantizedMeshParser: skirt vertices are generated");
    if (!mesh || firstSkirtVertex >= mesh->vertices.size()) return;

    const Vec3 normalDelta =
        mesh->vertices[firstSkirtVertex].normalEcef -
        mesh->vertices[0].normalEcef;
    check(normalDelta.length() < 1e-12,
          "QuantizedMeshParser: skirt normals copy source edge normals like cesium-native");
}

void testQuantizedMeshRtcOriginFromBoundingSphereCenter() {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const Rectangle bounds = scheme->tileToRectangle(rootKey);
    const Vec3 boundingSphereCenter(1234.0, -5678.0, 9012.0);
    const Vec3 tileCenter(9999.0, 8888.0, 7777.0);
    const std::vector<uint8_t> bytes =
        makeQuantizedMeshBytes(
            "", false, false, boundingSphereCenter, 0.0f, 100.0f, tileCenter);

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            bounds);

    check(mesh != nullptr,
          "QuantizedMeshParser: RTC-origin test mesh parses");
    check(mesh && mesh->hasLocalOriginEcef,
          "QuantizedMeshParser: quantized-mesh mesh center is exposed as RTC origin");
    const Vec3 originDelta =
        mesh ? (mesh->localOriginEcef - boundingSphereCenter) : Vec3::zero();
    check(originDelta.length() < 1e-12,
          "QuantizedMeshParser: RTC origin equals BoundingSphereCenter like cesium-native");

    const Vec3 expectedFirstVertex =
        Ellipsoid::WGS84().cartographicToCartesian(
            Cartographic::fromRadians(bounds.west(), bounds.south(), 0.0));
    const Vec3 vertexDelta =
        mesh ? (mesh->vertices[0].positionEcef - expectedFirstVertex)
             : Vec3::zero();
    check(vertexDelta.length() < 1e-6,
          "QuantizedMeshParser: mesh vertices stay absolute ECEF before upload");
}

void testQuantizedMeshHeaderHeightRangeIsExposed() {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const Rectangle bounds = scheme->tileToRectangle(rootKey);
    const float minimumHeight = -123.5f;
    const float maximumHeight = 456.25f;
    const std::vector<uint8_t> bytes = makeQuantizedMeshBytes(
        "", false, false, Vec3::zero(), minimumHeight, maximumHeight);

    std::unique_ptr<SurfaceTileMesh> mesh =
        QuantizedMeshParser::parseToSurfaceTileMesh(
            bytes.data(),
            bytes.size(),
            bounds);

    check(mesh != nullptr,
          "QuantizedMeshParser: height range test mesh parses");
    check(mesh && mesh->hasHeightRange,
          "QuantizedMeshParser: quantized-mesh header height range is exposed");
    check(mesh &&
              std::abs(mesh->minimumHeight - minimumHeight) < 1e-6 &&
              std::abs(mesh->maximumHeight - maximumHeight) < 1e-6,
          "QuantizedMeshParser: header min/max heights match cesium-native bounding region inputs");
}

void testTilesetUsesQuantizedMeshRtcOrigin() {
    auto provider = std::make_unique<QuantizedMeshTerrainProvider>(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    const Vec3 boundingSphereCenter(3456.0, -7890.0, 12345.0);
    const Vec3 tileCenter(-3456.0, 7890.0, -12345.0);
    auto heightmap = makeFlatHeightmap(0.0f);
    heightmap->rawData = makeQuantizedMeshBytes(
        "", false, false, boundingSphereCenter, 0.0f, 100.0f, tileCenter);
    TilesetTestAccess::putTerrainCache(tileset, rootKey, std::move(heightmap));

    check(root != nullptr,
          "Tileset: RTC-origin root tile is created");
    if (!root) return;

    TilesetTestAccess::ensureTileMesh(tileset, *root);
    const Vec3 originDelta =
        TilesetTestAccess::localOrigin(*root) - boundingSphereCenter;
    check(originDelta.length() < 1e-12,
          "Tileset: quantized-mesh BoundingSphereCenter wins over centroid RTC origin");
}

void testTilesetUsesQuantizedMeshHeightRange() {
    auto provider = std::make_unique<QuantizedMeshTerrainProvider>(
        "https://example.invalid/{z}/{x}/{y}.terrain");
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    const float minimumHeight = -250.0f;
    const float maximumHeight = 1789.0f;
    auto heightmap = makeFlatHeightmap(0.0f);
    heightmap->rawData = makeQuantizedMeshBytes(
        "", false, false, Vec3::zero(), minimumHeight, maximumHeight);
    TilesetTestAccess::putTerrainCache(tileset, rootKey, std::move(heightmap));

    check(root != nullptr,
          "Tileset: height-range root tile is created");
    if (!root) return;

    TilesetTestAccess::ensureTileMesh(tileset, *root);
    check(root->hasTerrainHeightRange,
          "Tileset: terrain tile stores quantized-mesh height range");
    check(std::abs(root->terrainMinimumHeight - minimumHeight) < 1e-6 &&
              std::abs(root->terrainMaximumHeight - maximumHeight) < 1e-6,
          "Tileset: quantized-mesh header min/max overrides heightmap fallback range");
}

void testTilesetBoundsUseQuantizedMeshHeightRange() {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey key{"Geographic-TMS", 5, 20, 12};
    const Rectangle bounds = scheme->tileToRectangle(key);
    const Vec3 center = TilesetTestAccess::tileBoundsCenter(bounds);

    TilesetTile looseTile(key, bounds);
    looseTile.hasTerrainHeightRange = true;
    looseTile.terrainMinimumHeight = -1000.0;
    looseTile.terrainMaximumHeight = 9000.0;

    TilesetTile exactTile(key, bounds);
    exactTile.hasTerrainHeightRange = true;
    exactTile.terrainMinimumHeight = -50.0;
    exactTile.terrainMaximumHeight = 1234.0;

    const double looseRadius =
        TilesetTestAccess::tileBoundsRadius(looseTile, center);
    const double exactRadius =
        TilesetTestAccess::tileBoundsRadius(exactTile, center);
    check(std::abs((looseRadius - exactRadius) - (9000.0 - 1234.0)) < 1e-6,
          "Tileset: terrain bounds radius uses exact QM height range instead of loose earth range");

    const double centerLng = (bounds.west() + bounds.east()) * 0.5;
    const double centerLat = (bounds.south() + bounds.north()) * 0.5;
    const auto& ellipsoid = Ellipsoid::WGS84();

    const Vec3 insideCamera = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(centerLng, centerLat, 100.0));
    const double insideDistance =
        TilesetTestAccess::approximateDistanceToTileBounds(
            exactTile,
            insideCamera);
    check(insideDistance < 1e-6,
          "Tileset: BoundingRegion distance is zero inside rectangle and height range");

    const Vec3 camera = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(centerLng, centerLat, 13000.0));
    const double looseDistance =
        TilesetTestAccess::approximateDistanceToTileBounds(
            looseTile,
            camera);
    const double exactDistance =
        TilesetTestAccess::approximateDistanceToTileBounds(
            exactTile,
            camera);
    check(std::abs((exactDistance - looseDistance) -
                   (9000.0 - 1234.0)) < 1e-6,
          "Tileset: fog/SSE distance uses BoundingRegion max-height range");

    const Vec3 outsideCamera = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(bounds.east() + bounds.width() * 0.25,
                                  centerLat,
                                  100.0));
    const double outsideDistance =
        TilesetTestAccess::approximateDistanceToTileBounds(
            exactTile,
            outsideCamera);
    check(outsideDistance > 1000.0,
          "Tileset: BoundingRegion distance includes horizontal rectangle-plane distance");

    const auto exactObb = TilesetTestAccess::tileBoundingRegionObb(exactTile);
    check(exactObb &&
              outsideDistance * outsideDistance + 1e-3 >=
                  exactObb->computeDistanceSquaredToPosition(outsideCamera),
          "Tileset: BoundingRegion distance is at least OBB distance like cesium-native");

    const double centerFallbackDistance =
        TilesetTestAccess::approximateDistanceToTileBounds(
            exactTile,
            Vec3::zero());
    const double centerObbDistance = exactObb
        ? std::sqrt(exactObb->computeDistanceSquaredToPosition(Vec3::zero()))
        : -1.0;
    check(exactObb &&
              std::abs(centerFallbackDistance - centerObbDistance) < 1e-6,
          "Tileset: BoundingRegion center-position distance falls back to OBB like cesium-native");
}

void testTilesetBoundingRegionObbUsesQuantizedMeshHeightRange() {
    auto scheme = TileScheme::createGeographicTMS();
    const TileKey key{"Geographic-TMS", 6, 40, 24};
    const Rectangle bounds = scheme->tileToRectangle(key);

    TilesetTile looseTile(key, bounds);
    looseTile.hasTerrainHeightRange = true;
    looseTile.terrainMinimumHeight = -1000.0;
    looseTile.terrainMaximumHeight = 9000.0;

    TilesetTile exactTile(key, bounds);
    exactTile.hasTerrainHeightRange = true;
    exactTile.terrainMinimumHeight = -50.0;
    exactTile.terrainMaximumHeight = 1234.0;

    const auto looseObb =
        TilesetTestAccess::tileBoundingRegionObb(looseTile);
    const auto exactObb =
        TilesetTestAccess::tileBoundingRegionObb(exactTile);
    check(looseObb.has_value() && exactObb.has_value(),
          "Tileset: BoundingRegion OBB builds for quadtree terrain tile");
    if (!looseObb || !exactObb) return;

    const double looseVerticalHalfAxis =
        looseObb->getHalfAxis(2).length();
    const double exactVerticalHalfAxis =
        exactObb->getHalfAxis(2).length();
    check(exactVerticalHalfAxis < looseVerticalHalfAxis,
          "Tileset: BoundingRegion OBB vertical extent uses exact QM height range");

    const auto& ellipsoid = Ellipsoid::WGS84();
    const double centerLng = bounds.west() + bounds.width() * 0.5;
    const double centerLat = (bounds.south() + bounds.north()) * 0.5;
    const Vec3 tangentPoint = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(centerLng, centerLat, 0.0));
    const Vec3 origin = ellipsoid.scaleToGeodeticSurface(tangentPoint);
    const Mat4 tangentFrame =
        Transforms::eastNorthUpToFixedFrame(origin, ellipsoid);
    const Vec3 expectedEast(tangentFrame(0, 0),
                            tangentFrame(1, 0),
                            tangentFrame(2, 0));
    const Vec3 expectedNorth(tangentFrame(0, 1),
                             tangentFrame(1, 1),
                             tangentFrame(2, 1));
    const Vec3 expectedUp(tangentFrame(0, 2),
                          tangentFrame(1, 2),
                          tangentFrame(2, 2));
    check(exactObb->getHalfAxis(0).normalized().dot(expectedEast) > 1.0 - 1e-12 &&
              exactObb->getHalfAxis(1).normalized().dot(expectedNorth) > 1.0 - 1e-12 &&
              exactObb->getHalfAxis(2).normalized().dot(expectedUp) > 1.0 - 1e-12,
          "Tileset: BoundingRegion OBB axes use cesium-native ENU tangent frame");

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(bounds);
    Camera camera;
    camera.lookAt(center + center.normalized() * 200000.0,
                  center,
                  Vec3::unitZ());
    const Frustum frustum = camera.frustum(800.0, 800.0);
    check(TilesetTestAccess::tileIntersectsFrustum(exactTile, frustum),
          "Tileset: BoundingRegion OBB participates in frustum visibility");
}

void testTilesetBoundingRegionObbHandlesLargeRectanglesLikeCesiumNative() {
    constexpr double kPi = 3.14159265358979323846264338327950288;
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const Rectangle bounds(-kPi, -kPi * 0.5, kPi, kPi * 0.5);

    TilesetTile rootTile(rootKey, bounds);
    rootTile.hasTerrainHeightRange = true;
    rootTile.terrainMinimumHeight = -1000.0;
    rootTile.terrainMaximumHeight = 9000.0;

    const auto rootObb = TilesetTestAccess::tileBoundingRegionObb(rootTile);
    check(rootObb.has_value(),
          "Tileset: BoundingRegion OBB builds for width greater than PI like cesium-native");
    if (!rootObb) return;

    const double equatorialExtent =
        Ellipsoid::WGS84().semiMajorAxis() + rootTile.terrainMaximumHeight;
    const double polarExtent =
        Ellipsoid::WGS84().semiMinorAxis() + rootTile.terrainMaximumHeight;
    const double centerError = rootObb->getCenter().length();
    const double axis0Error =
        std::abs(rootObb->getHalfAxis(0).length() - equatorialExtent);
    const double axis1Error =
        std::abs(rootObb->getHalfAxis(1).length() - polarExtent);
    const double axis2Error =
        std::abs(rootObb->getHalfAxis(2).length() - equatorialExtent);
    check(centerError < 1e-6 &&
              axis0Error < 1e-6 &&
              axis1Error < 1e-6 &&
              axis2Error < 1e-6,
          "Tileset: large BoundingRegion OBB extents match cesium-native root-region strategy");
}

void testQuantizedMeshLayerJsonVersionAndExtensionQuery() {
    QuantizedMeshTerrainProvider provider("https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "version": "1.33.0",
      "tiles": ["{z}/{x}/{y}.terrain?v={version}"],
      "extensions": ["metadata", "watermask", "octvertexnormals"],
      "minzoom": 0,
      "maxzoom": 4
    })json";

    check(provider.configureFromLayerJson(
              layerJson, "https://example.invalid/terrain/layer.json"),
          "QuantizedMeshTerrainProvider: versioned extension layer configures");
    check(provider.buildUrl(TileKey{"Geographic-TMS", 2, 3, 1}) ==
              "https://example.invalid/terrain/2/3/1.terrain?v=1.33.0&extensions=octvertexnormals-metadata",
          "QuantizedMeshTerrainProvider: version and metadata extensions match cesium-native");

    QuantizedMeshTerrainProvider overrideProvider("https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string overrideLayerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain?extensions=old&v={version}"],
      "version": "2",
      "extensions": ["metadata"],
      "minzoom": 0,
      "maxzoom": 4
    })json";
    check(overrideProvider.configureFromLayerJson(
              overrideLayerJson, "https://example.invalid/terrain/layer.json"),
          "QuantizedMeshTerrainProvider: existing extension query layer configures");
    check(overrideProvider.buildUrl(TileKey{"Geographic-TMS", 1, 1, 0}) ==
              "https://example.invalid/terrain/1/1/0.terrain?extensions=metadata&v=2",
          "QuantizedMeshTerrainProvider: extensions query is replaced like cesium-native UriQuery");

    QuantizedMeshTerrainProvider unknownProvider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string unknownLayerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{unknown}/{y}.terrain?bad={unterminated"],
      "minzoom": 0,
      "maxzoom": 4
    })json";
    check(unknownProvider.configureFromLayerJson(
              unknownLayerJson, "https://example.invalid/terrain/layer.json"),
          "QuantizedMeshTerrainProvider: unknown placeholder layer configures");
    check(unknownProvider.buildUrl(TileKey{"Geographic-TMS", 2, 3, 1}) ==
              "https://example.invalid/terrain/2/3/unknown/1.terrain?bad={unterminated",
          "QuantizedMeshTerrainProvider: unknown and malformed placeholders match cesium-native substitution");
}

void testQuantizedMeshLayerJsonUriResolution() {
    QuantizedMeshTerrainProvider relativeProvider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string relativeLayerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["../tiles/{level}/{x}/{y}.terrain?token=tile&v={version}"],
      "version": "7",
      "minzoom": 0,
      "maxzoom": 4
    })json";

    check(relativeProvider.configureFromLayerJson(
              relativeLayerJson,
              "https://example.invalid/terrain/layers/main/layer.json?asset=123&token=base"),
          "QuantizedMeshTerrainProvider: relative URI layer configures");
    check(relativeProvider.buildUrl(TileKey{"Geographic-TMS", 3, 5, 2}) ==
              "https://example.invalid/terrain/layers/tiles/3/5/2.terrain?token=tile&v=7&asset=123",
          "QuantizedMeshTerrainProvider: URI resolve handles parent dirs, {level}, and base query merge");

    QuantizedMeshTerrainProvider rootProvider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string rootLayerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["/terrain/{z}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 4
    })json";

    check(rootProvider.configureFromLayerJson(
              rootLayerJson,
              "https://assets.example.invalid/ion/world/layer.json?access_token=abc"),
          "QuantizedMeshTerrainProvider: root-relative URI layer configures");
    check(rootProvider.buildUrl(TileKey{"Geographic-TMS", 1, 1, 0}) ==
              "https://assets.example.invalid/terrain/1/1/0.terrain?access_token=abc",
          "QuantizedMeshTerrainProvider: root-relative URI keeps layer query like cesium-native");

    QuantizedMeshTerrainProvider absoluteProvider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string absoluteLayerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["https://cdn.example.invalid/qm/{level}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 4
    })json";

    check(absoluteProvider.configureFromLayerJson(
              absoluteLayerJson,
              "https://assets.example.invalid/layer.json?token=base"),
          "QuantizedMeshTerrainProvider: absolute URI layer configures");
    check(absoluteProvider.buildUrl(TileKey{"Geographic-TMS", 2, 3, 1}) ==
              "https://cdn.example.invalid/qm/2/3/1.terrain?token=base",
          "QuantizedMeshTerrainProvider: absolute template inherits layer query like cesium-native");
}

void testQuantizedMeshFabdemLayerJsonShape() {
    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "attribution": "FABDEM V1-2, University of Bristol, CC BY-NC-SA 4.0",
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}],
        [], [], [], [], [], [], [], [], [], [], [],
        [{"startX":6487,"startY":2685,"endX":6606,"endY":2784}]
      ],
      "bounds": [105.17, 28.1, 110.2, 32.25],
      "format": "quantized-mesh-1.0",
      "maxzoom": 12,
      "minzoom": 0,
      "name": "FABDEM Quantized Mesh",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tilejson": "2.1.0",
      "tiles": ["./{z}/{x}/{y}.terrain"],
      "version": "1.0.0"
    })json";

    check(provider.configureFromLayerJson(
              layerJson, "http://192.168.1.8:8092/layer.json"),
          "QuantizedMeshTerrainProvider: FABDEM layer.json shape configures");
    check(provider.maxZoom() == 12,
          "QuantizedMeshTerrainProvider: FABDEM maxzoom is retained");
    check(provider.supportsTile(TileKey{"Geographic-TMS", 0, 0, 0}) &&
              provider.supportsTile(TileKey{"Geographic-TMS", 0, 1, 0}),
          "QuantizedMeshTerrainProvider: FABDEM level-0 grid is 2x1");
    const TileKey sample{"Geographic-TMS", 12, 6487, 2685};
    check(provider.supportsTile(sample),
          "QuantizedMeshTerrainProvider: FABDEM sample tile is available");
    check(provider.buildUrl(sample) ==
              "http://192.168.1.8:8092/12/6487/2685.terrain",
          "QuantizedMeshTerrainProvider: FABDEM ./ tile template resolves");
    check(!provider.supportsTile(TileKey{"Geographic-TMS", 12, 6486, 2685}),
          "QuantizedMeshTerrainProvider: FABDEM outside available range is unavailable");
    check(provider.availabilityState(TileKey{"Geographic-TMS", 13, 12974, 5370}) ==
              TileAvailabilityState::NotAvailable,
          "QuantizedMeshTerrainProvider: FABDEM level 12 availability does not imply level 13");
}

void testQuantizedMeshParentLayerFallback() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth_md_qm_parent_layer_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "child");
    std::filesystem::create_directories(root / "parent");

    const std::string parentLayerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["parentTiles/{z}/{x}/{y}.terrain?source=parent"],
      "minzoom": 0,
      "maxzoom": 4,
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}],
        [{"startX":2,"startY":0,"endX":2,"endY":0}]
      ]
    })json";
    {
        std::ofstream out(root / "parent" / "layer.json");
        out << parentLayerJson;
    }

    const std::string childLayerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["childTiles/{level}/{x}/{y}.terrain?v={version}"],
      "version": "9",
      "parentUrl": "../parent",
      "minzoom": 0,
      "maxzoom": 4,
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}],
        [{"startX":0,"startY":0,"endX":0,"endY":0}]
      ]
    })json";

    const std::string childLayerUrl =
        "file://" + (root / "child" / "layer.json").generic_string();
    const std::string childBase =
        "file://" + (root / "child").generic_string();
    const std::string parentBase =
        "file://" + (root / "parent").generic_string();

    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    check(provider.configureFromLayerJson(childLayerJson, childLayerUrl),
          "QuantizedMeshTerrainProvider: parentUrl layer configures");

    const TileKey childTile{"Geographic-TMS", 1, 0, 0};
    const TileKey parentTile{"Geographic-TMS", 1, 2, 0};
    const TileKey missingTile{"Geographic-TMS", 1, 3, 0};

    check(provider.supportsTile(childTile),
          "QuantizedMeshTerrainProvider: top layer available tile is supported");
    check(provider.buildUrl(childTile) ==
              childBase + "/childTiles/1/0/0.terrain?v=9",
          "QuantizedMeshTerrainProvider: top layer wins before parent layer");
    check(provider.supportsTile(parentTile),
          "QuantizedMeshTerrainProvider: parent layer fills top-layer gap");
    check(provider.buildUrl(parentTile) ==
              parentBase + "/parentTiles/1/2/0.terrain?source=parent",
          "QuantizedMeshTerrainProvider: unavailable top tile falls back to parent URL");
    check(provider.availabilityState(missingTile) ==
              TileAvailabilityState::NotAvailable,
          "QuantizedMeshTerrainProvider: tile missing from all layers is not available");

    std::filesystem::remove_all(root);
}

void testQuantizedMeshLoadsUnderlyingLayerAvailabilityWithTile() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth_md_qm_underlying_availability_test";
    std::filesystem::remove_all(root);

    const std::string parentLayerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["parentTiles/{z}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 4,
      "metadataAvailability": 1,
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}]
      ]
    })json";
    {
        std::filesystem::create_directories(root / "parent");
        std::ofstream out(root / "parent" / "layer.json");
        out << parentLayerJson;
    }

    const std::string childLayerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["childTiles/{z}/{x}/{y}.terrain"],
      "parentUrl": "../parent",
      "minzoom": 0,
      "maxzoom": 4,
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}]
      ]
    })json";

    const std::string parentMetadata = R"json({
      "available": [
        [{"startX":2,"startY":0,"endX":2,"endY":0}]
      ]
    })json";

    writeBytes(root / "child" / "childTiles" / "0" / "0" / "0.terrain",
               makeQuantizedMeshBytes());
    writeBytes(root / "parent" / "parentTiles" / "0" / "0" / "0.terrain",
               makeQuantizedMeshBytes(parentMetadata));

    const std::string childLayerUrl =
        "file://" + (root / "child" / "layer.json").generic_string();
    const std::string parentBase =
        "file://" + (root / "parent").generic_string();

    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    check(provider.configureFromLayerJson(childLayerJson, childLayerUrl),
          "QuantizedMeshTerrainProvider: underlying availability layer configures");

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey parentOnlyChild{"Geographic-TMS", 1, 2, 0};

    check(provider.availabilityState(parentOnlyChild) ==
              TileAvailabilityState::Unknown,
          "QuantizedMeshTerrainProvider: parent child is unknown before metadata tile loads");

    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::unique_ptr<DecodedHeightmap> heightmap;
    provider.requestTile(
        rootKey,
        CancellationToken{},
        [&](const TileKey&, TerrainTileLoadResult result) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                heightmap = std::move(result.heightmap);
                done = true;
            }
            cv.notify_one();
        });

    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(5), [&] { return done; });
    }

    check(heightmap != nullptr,
          "QuantizedMeshTerrainProvider: root tile request produced heightmap");
    if (heightmap) {
        provider.applyAvailabilityUpdates(*heightmap);
    }

    check(provider.supportsTile(parentOnlyChild),
          "QuantizedMeshTerrainProvider: underlying metadata makes parent child available");
    check(provider.buildUrl(parentOnlyChild) ==
              parentBase + "/parentTiles/1/2/0.terrain",
          "QuantizedMeshTerrainProvider: newly available child uses underlying layer URL");

    std::filesystem::remove_all(root);
}

std::unique_ptr<DecodedHeightmap> makeFlatHeightmap(float heightMeters);

void testTilesetDefersAvailabilityBoundaryChildrenUntilContentLoaded() {
    auto provider = std::make_unique<QuantizedMeshTerrainProvider>(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 4,
      "metadataAvailability": 1
    })json";

    check(provider->configureFromLayerJson(
              layerJson, "https://example.invalid/layer.json"),
          "Tileset: availability-boundary layer configures");

    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: availability-boundary root tile is created");

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    check(root->children.empty(),
          "Tileset: availability-boundary children wait for parent content");

    const std::string rootMetadata = R"json({
      "available": [
        [{"startX":0,"startY":0,"endX":0,"endY":0}]
      ]
    })json";
    auto rootHeightmap = makeFlatHeightmap(10.0f);
    rootHeightmap->rawData = makeQuantizedMeshBytes(rootMetadata);
    TilesetTestAccess::putTerrainCache(tileset, rootKey, std::move(rootHeightmap));
    TilesetTestAccess::ensureTileMesh(tileset, *root);
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    check(root->children.size() == 4,
          "Tileset: availability-boundary children appear after metadata content loads");
}

class DefaultZoomTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "default-zoom-terrain"; }
    std::string schemeId() const override { return "Geographic-TMS"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }

    std::string buildUrl(const TileKey&) const override {
        return "memory://default-zoom";
    }

    void requestTile(const TileKey& key,
                     CancellationToken,
                     HeightmapCallback callback) override {
        ++requestCount;
        callback(key, TerrainTileLoadResult::retryLater());
    }

    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

    int requestCount = 0;
};

class SparseTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "sparse-terrain"; }
    std::string schemeId() const override { return "Geographic-TMS"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 4; }
    int tileSize() const override { return 2; }

    TileAvailabilityState availabilityState(const TileKey& key) const override {
        if (key.schemeId != schemeId()) return TileAvailabilityState::NotAvailable;
        if (key.z == 0) return TileAvailabilityState::Available;
        if (key.z == 1 && key.x == 0 && key.y == 0) {
            return TileAvailabilityState::Available;
        }
        return TileAvailabilityState::NotAvailable;
    }

    std::string buildUrl(const TileKey&) const override {
        return "memory://sparse";
    }

    void requestTile(const TileKey& key,
                     CancellationToken,
                     HeightmapCallback callback) override {
        ++requestCount;
        callback(key, TerrainTileLoadResult::retryLater());
    }

    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

    int requestCount = 0;
};

class ExplicitPastMaxZoomTerrainProvider final : public TerrainProvider {
public:
    explicit ExplicitPastMaxZoomTerrainProvider(
        std::shared_ptr<std::vector<TileKey>> requestedKeys)
        : requestedKeys_(std::move(requestedKeys)) {}

    std::string id() const override { return "explicit-past-maxzoom-terrain"; }
    std::string schemeId() const override { return "Geographic-TMS"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }

    TileAvailabilityState availabilityState(const TileKey& key) const override {
        if (key.schemeId != schemeId()) return TileAvailabilityState::NotAvailable;
        if (key.z == 2 && key.x == 0 && key.y == 0) {
            return TileAvailabilityState::Available;
        }
        return TerrainProvider::availabilityState(key);
    }

    std::string buildUrl(const TileKey&) const override {
        return "memory://explicit-past-maxzoom";
    }

    void requestTile(const TileKey& key,
                     CancellationToken,
                     HeightmapCallback callback) override {
        requestedKeys_->push_back(key);
        callback(key, TerrainTileLoadResult::retryLater());
    }

    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

private:
    std::shared_ptr<std::vector<TileKey>> requestedKeys_;
};

class TerminalTerrainProvider final : public TerrainProvider {
public:
    explicit TerminalTerrainProvider(TerrainTileLoadStatus status)
        : status_(status) {}

    std::string id() const override { return "terminal-terrain"; }
    std::string schemeId() const override { return "Geographic-TMS"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }

    std::string buildUrl(const TileKey&) const override {
        return "memory://terminal";
    }

    void requestTile(const TileKey& key,
                     CancellationToken,
                     HeightmapCallback callback) override {
        ++requestCount;
        switch (status_) {
            case TerrainTileLoadStatus::Success:
                callback(key, TerrainTileLoadResult::success(makeHeightmap()));
                break;
            case TerrainTileLoadStatus::Empty:
                callback(key, TerrainTileLoadResult::empty());
                break;
            case TerrainTileLoadStatus::RetryLater:
                callback(key, TerrainTileLoadResult::retryLater());
                break;
            case TerrainTileLoadStatus::Failed:
                callback(key, TerrainTileLoadResult::failed());
                break;
            case TerrainTileLoadStatus::Cancelled:
                callback(key, TerrainTileLoadResult::cancelled());
                break;
        }
    }

    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

    int requestCount = 0;

private:
    static std::unique_ptr<DecodedHeightmap> makeHeightmap() {
        auto hm = std::make_unique<DecodedHeightmap>();
        hm->tileSize = 2;
        hm->heights = {0.0f, 0.0f, 0.0f, 0.0f};
        return hm;
    }

    TerrainTileLoadStatus status_;
};

class ManualCompletionTerrainProvider final : public TerrainProvider {
public:
    struct PendingRequest {
        TileKey key;
        HeightmapCallback callback;
    };

    std::string id() const override { return "manual-completion-terrain"; }
    std::string schemeId() const override { return "Geographic-TMS"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }

    std::string buildUrl(const TileKey&) const override {
        return "memory://manual-completion";
    }

    void requestTile(const TileKey& key,
                     CancellationToken,
                     HeightmapCallback callback) override {
        pendingRequests.push_back(PendingRequest{key, std::move(callback)});
    }

    bool completeWithHeightmap(
        const TileKey& key,
        std::unique_ptr<DecodedHeightmap> heightmap) {
        auto it = std::find_if(
            pendingRequests.begin(),
            pendingRequests.end(),
            [&key](const PendingRequest& request) {
                return request.key == key;
            });
        if (it == pendingRequests.end()) {
            return false;
        }

        HeightmapCallback callback = std::move(it->callback);
        pendingRequests.erase(it);
        callback(key, TerrainTileLoadResult::success(std::move(heightmap)));
        return true;
    }

    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*, size_t) override {
        return nullptr;
    }

    std::vector<PendingRequest> pendingRequests;
};

class ManualCompletionContentProvider final : public TilesetContentProvider {
public:
    struct PendingRequest {
        TileKey key;
        ContentCallback callback;
    };

    explicit ManualCompletionContentProvider(TileKey key)
        : key_(std::move(key)) {}

    std::string id() const override {
        return "manual-completion-content";
    }

    bool supportsTile(const TileKey& key) const override {
        return key == key_;
    }

    std::vector<TileKey> rootTiles() const override {
        return {key_};
    }

    void requestTileContent(const TileKey& key,
                            CancellationToken,
                            ContentCallback callback) override {
        pendingRequests.push_back(PendingRequest{key, std::move(callback)});
    }

    bool completeWithModel(
        const TileKey& key,
        std::unique_ptr<GltfModel> model) {
        auto it = std::find_if(
            pendingRequests.begin(),
            pendingRequests.end(),
            [&key](const PendingRequest& request) {
                return request.key == key;
            });
        if (it == pendingRequests.end()) {
            return false;
        }

        ContentCallback callback = std::move(it->callback);
        pendingRequests.erase(it);
        callback(key, TileContentLoadResult::render(std::move(model)));
        return true;
    }

    TileContentLoadResult decodeContent(
        const uint8_t*,
        size_t) override {
        return TileContentLoadResult::failed();
    }

    std::vector<PendingRequest> pendingRequests;

private:
    TileKey key_;
};

std::unique_ptr<DecodedHeightmap> makeFlatHeightmap(float heightMeters) {
    auto hm = std::make_unique<DecodedHeightmap>();
    hm->tileSize = 2;
    hm->heights = {heightMeters, heightMeters, heightMeters, heightMeters};
    hm->minHeight = heightMeters;
    hm->maxHeight = heightMeters;
    return hm;
}

void testTilesetMissingRasterProjectionUnloadsRenderContent() {
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        makeRasterOverlayOptions());
    auto overlayProvider = std::make_unique<RasterOverlayTileProvider>(
        overlay->getProvider(),
        overlay->getTileScheme(),
        nullptr);
    ActivatedRasterOverlay activated(*overlay);
    activated.setTileProvider(std::move(overlayProvider));

    auto terrainProvider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::move(terrainProvider),
        std::move(scheme),
        {&activated},
        nullptr,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: missing-raster-projection root tile is created");
    if (!root) return;

    root->mesh = std::make_unique<SurfaceTileMesh>();
    root->meshReady = true;
    root->gpuVertexBuffer = std::make_unique<DummyBuffer>(32);
    root->loadState = TileLoadState::Done;
    root->contentKind = TileContentKind::Render;
    root->rasterOverlays.resize(1);

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);

    check(commands.empty(),
          "Tileset: missing raster projection stops draw command creation");
    check(root->loadState == TileLoadState::Unloaded &&
              root->contentKind == TileContentKind::Unknown,
          "Tileset: missing raster projection unloads render content like cesium-native");
    check(!root->meshReady && root->mesh == nullptr &&
              root->gpuVertexBuffer == nullptr,
          "Tileset: missing raster projection releases render resources before reload");
    check(root->rasterOverlays.empty(),
          "Tileset: missing raster projection clears mapped raster state before reload");
    check(root->missingRasterOverlayProjections.size() == 1 &&
              root->missingRasterOverlayProjections.front() ==
                  RasterOverlayProjection::Geographic,
          "Tileset: missing raster projection remains diagnosable after unload");
}

void testTilesetRasterTargetPixelsUseRenderContentRectangle() {
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        makeRasterOverlayOptions());
    auto overlayProvider = std::make_unique<RasterOverlayTileProvider>(
        overlay->getProvider(),
        overlay->getTileScheme(),
        nullptr);
    ActivatedRasterOverlay activated(*overlay);
    activated.setTileProvider(std::move(overlayProvider));

    auto terrainProvider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::move(terrainProvider),
        std::move(scheme),
        {&activated},
        nullptr,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: raster target-pixels root tile is created");
    if (!root) return;

    const Rectangle preciseRectangle =
        Rectangle::fromDegrees(-10.0, -5.0, -2.0, 5.0);
    root->mesh = std::make_unique<SurfaceTileMesh>();
    root->mesh->rasterOverlayDetails.setGeographicRectangle(preciseRectangle);
    root->meshReady = true;
    root->gpuVertexBuffer = std::make_unique<DummyBuffer>(32);
    root->loadState = TileLoadState::Done;
    root->contentKind = TileContentKind::Render;
    root->rasterOverlays.resize(1);

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);

    RasterMappedToTilesetTile* mapped = root->rasterOverlays[0].get();
    RasterOverlayTile* loadingTile = mapped ? mapped->getLoadingTile() : nullptr;
    check(loadingTile != nullptr &&
              loadingTile->getRectangle() == preciseRectangle,
          "Tileset: raster target-pixels request uses render-content rectangle");
    check(loadingTile != nullptr &&
              loadingTile->getTargetScreenPixelsX() < 80.0 &&
              loadingTile->getTargetScreenPixelsY() < 80.0,
          "Tileset: raster target-pixels are computed from precise rectangle size");
}

void testTilesetGltfRenderContentBuildsPrimitiveCommands() {
    DummyRenderDevice device;
    device.allowTextureCreation = true;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: glTF render-content root tile is created");
    if (!root) return;

    root->gltfModel = makeTexturedTriangleGltfModel();
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::ContentLoaded;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        0.5f);

    check(root->meshReady && root->loadState == TileLoadState::Done,
          "Tileset: glTF render content reaches ready state after resource upload");
    check(root->mesh == nullptr && root->gpuVertexBuffer == nullptr,
          "Tileset: glTF render content does not synthesize terrain mesh resources");
    check(root->gltfPrimitiveResources.size() == 1,
          "Tileset: glTF render content creates one primitive resource set");
    check(device.createdTextureCount == 1,
          "Tileset: glTF baseColorTexture is uploaded as a GPU texture");
    check(device.lastTextureDesc.width == 1 &&
              device.lastTextureDesc.height == 1 &&
              device.lastTextureDesc.wrapS == TextureDesc::Wrap::MirroredRepeat &&
              device.lastTextureDesc.wrapT == TextureDesc::Wrap::Clamp,
          "Tileset: glTF sampler state is preserved when uploading texture");
    check(commands.size() == 1,
          "Tileset: glTF render content emits one primitive draw command");
    if (commands.empty()) return;

    const RenderCommand& cmd = commands.front();
    check(cmd.kind == RenderCommandKind::GltfPrimitive,
          "Tileset: glTF draw command uses the GltfPrimitive kind");
    check(cmd.vertexStride == 120 && cmd.indexCount == 3 &&
              cmd.vertexCount == 3,
          "Tileset: glTF draw command preserves primitive geometry counts");
    check(cmd.vertexBuffer ==
              root->gltfPrimitiveResources.front().vertexBuffer.get() &&
              cmd.indexBuffer ==
              root->gltfPrimitiveResources.front().indexBuffer.get(),
          "Tileset: glTF draw command references primitive render resources");
    const auto* uploadedVertices =
        dynamic_cast<const DummyBuffer*>(cmd.vertexBuffer);
    check(uploadedVertices && uploadedVertices->size() == 3u * 120u,
          "Tileset: glTF vertex buffer uses multi-UV/COLOR_0/TANGENT layout bytes");
    bool firstVertexSecondUvUploaded = false;
    bool firstVertexColorUploaded = false;
    bool firstVertexTangentUploaded = false;
    if (uploadedVertices && uploadedVertices->bytes().size() >= 120u) {
        std::array<float, 30> values{};
        std::memcpy(
            values.data(),
            uploadedVertices->bytes().data(),
            values.size() * sizeof(float));
        firstVertexSecondUvUploaded =
            std::abs(values[8] - 0.25f) < 1e-6f &&
            std::abs(values[9] - 0.75f) < 1e-6f;
        firstVertexColorUploaded =
            std::abs(values[10] - 0.25f) < 1e-6f &&
            std::abs(values[11] - 0.5f) < 1e-6f &&
            std::abs(values[12] - 0.75f) < 1e-6f &&
            std::abs(values[13] - 1.0f) < 1e-6f;
        firstVertexTangentUploaded =
            std::abs(values[14] - 1.0f) < 1e-6f &&
            std::abs(values[15] - 0.0f) < 1e-6f &&
            std::abs(values[16] - 0.0f) < 1e-6f &&
            std::abs(values[17] - 1.0f) < 1e-6f;
    }
    check(firstVertexSecondUvUploaded,
          "Tileset: glTF GPU vertices include TEXCOORD_1 values");
    check(firstVertexColorUploaded,
          "Tileset: glTF GPU vertices include COLOR_0 values");
    check(firstVertexTangentUploaded,
          "Tileset: glTF GPU vertices include TANGENT values");
    check(cmd.textures.size() >= 1 &&
              cmd.textures.front() ==
                  root->gltfPrimitiveResources.front().baseColorTexture.texture &&
              cmd.uniforms.count("u_hasBaseColorTexture") &&
              cmd.uniforms.at("u_hasBaseColorTexture").front() == 1.0f &&
              cmd.uniforms.count("u_textureCoordSets") &&
              cmd.uniforms.at("u_textureCoordSets").size() == 4 &&
              cmd.uniforms.at("u_textureCoordSets")[0] == 1.0f,
          "Tileset: glTF draw command binds baseColorTexture with TEXCOORD_1");
    check(cmd.uniforms.count("u_baseColorTexOffsetScale") &&
              cmd.uniforms.at("u_baseColorTexOffsetScale").size() == 4 &&
              std::abs(cmd.uniforms.at("u_baseColorTexOffsetScale")[0] - 0.25f) <
                  1e-6f &&
              std::abs(cmd.uniforms.at("u_baseColorTexOffsetScale")[1] - 0.5f) <
                  1e-6f &&
              std::abs(cmd.uniforms.at("u_baseColorTexOffsetScale")[2] - 2.0f) <
                  1e-6f &&
              std::abs(cmd.uniforms.at("u_baseColorTexOffsetScale")[3] - 3.0f) <
                  1e-6f &&
              cmd.uniforms.count("u_baseColorTexRotationSinCos") &&
              cmd.uniforms.at("u_baseColorTexRotationSinCos").size() == 2 &&
              std::abs(cmd.uniforms.at("u_baseColorTexRotationSinCos")[0] - 1.0f) <
                  1e-6f &&
              std::abs(cmd.uniforms.at("u_baseColorTexRotationSinCos")[1]) <
                  1e-6f,
          "Tileset: glTF draw command uploads KHR_texture_transform uniforms");
    check(cmd.blend &&
              !cmd.depthWrite &&
              cmd.uniforms.count("u_renderOpacity") &&
              std::abs(cmd.uniforms.at("u_renderOpacity").front() - 0.5f) <
                  1e-6f,
          "Tileset: glTF draw command applies LOD transition opacity");
    check(cmd.uniforms.count("u_modelOrigin") &&
              cmd.uniforms.at("u_modelOrigin").size() == 3,
          "Tileset: glTF draw command exposes RTC model origin to Scene MVP");
    check(cmd.hasWorldSortCenter &&
              std::abs(cmd.worldSortCenter[0] -
                       root->gltfPrimitiveResources.front().sortCenterEcef.x()) <
                  1e-9 &&
              std::abs(cmd.worldSortCenter[1] -
                       root->gltfPrimitiveResources.front().sortCenterEcef.y()) <
                  1e-9 &&
              std::abs(cmd.worldSortCenter[2] -
                       root->gltfPrimitiveResources.front().sortCenterEcef.z()) <
                  1e-9 &&
              std::abs(cmd.worldSortCenter[0] - (1.0 / 3.0)) < 1e-9 &&
              std::abs(cmd.worldSortCenter[1] - (1.0 / 3.0)) < 1e-9 &&
              std::abs(cmd.worldSortCenter[2]) < 1e-9,
          "Tileset: glTF draw command carries primitive world center for transparent sorting");
}

void testTilesetGltfTangentsUseModelLinearTransform() {
    DummyRenderDevice device;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: glTF tangent transform root tile is created");
    if (!root) return;

    root->gltfModel = makeTriangleGltfModel();
    const float diagonal = static_cast<float>(1.0 / std::sqrt(2.0));
    for (auto& tangent : root->gltfModel->primitives[0].vertexTangents) {
        tangent = {diagonal, diagonal, 0.0f, 1.0f};
    }
    root->gltfContentTransform = Mat4::scale(Vec3(2.0, 1.0, 1.0));
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::ContentLoaded;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);

    check(!commands.empty() && root->gltfPrimitiveResources.size() == 1u,
          "Tileset: glTF tangent transform uploads primitive resources");
    if (commands.empty()) return;

    const auto* uploadedVertices =
        dynamic_cast<const DummyBuffer*>(commands.front().vertexBuffer);
    bool tangentUsesModelLinear = false;
    if (uploadedVertices && uploadedVertices->bytes().size() >= 120u) {
        std::array<float, 30> values{};
        std::memcpy(
            values.data(),
            uploadedVertices->bytes().data(),
            values.size() * sizeof(float));
        tangentUsesModelLinear =
            std::abs(values[14] - 0.8944272f) < 1e-5f &&
            std::abs(values[15] - 0.4472136f) < 1e-5f &&
            std::abs(values[16]) < 1e-6f &&
            std::abs(values[17] - 1.0f) < 1e-6f;
    }
    check(tangentUsesModelLinear,
          "Tileset: glTF tangents use model linear transform under non-uniform scale");
}

void testTilesetGltfMaskMaterialStaysOpaqueCommand() {
    DummyRenderDevice device;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: glTF MASK material root tile is created");
    if (!root) return;

    root->gltfModel = makeTriangleGltfModel();
    root->gltfModel->primitives[0].alphaMode = GltfAlphaMode::Mask;
    root->gltfModel->primitives[0].alphaCutoff = 0.35f;
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::ContentLoaded;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);

    check(commands.size() == 1,
          "Tileset: glTF MASK material emits one primitive draw command");
    if (commands.empty()) return;

    const RenderCommand& cmd = commands.front();
    check(!cmd.blend && cmd.depthWrite,
          "Tileset: glTF MASK material remains an opaque depth-writing command");
    check(cmd.uniforms.count("u_alphaMode") &&
              cmd.uniforms.at("u_alphaMode").front() == 1.0f &&
              cmd.uniforms.count("u_alphaCutoff") &&
              std::abs(cmd.uniforms.at("u_alphaCutoff").front() - 0.35f) <
                  1e-6f,
          "Tileset: glTF MASK material uploads alpha cutoff uniforms");
}

void testTilesetGltfUnlitMaterialUploadsUniform() {
    DummyRenderDevice device;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: glTF unlit material root tile is created");
    if (!root) return;

    root->gltfModel = makeTriangleGltfModel();
    root->gltfModel->primitives[0].unlit = true;
    root->gltfModel->primitives[0].metallicFactor = 0.9f;
    root->gltfModel->primitives[0].roughnessFactor = 0.1f;
    root->gltfModel->primitives[0].emissiveFactor = {0.4f, 0.5f, 0.6f};
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::ContentLoaded;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);

    check(commands.size() == 1,
          "Tileset: glTF unlit material emits one primitive draw command");
    if (commands.empty()) return;

    const RenderCommand& cmd = commands.front();
    check(cmd.uniforms.count("u_unlit") &&
              cmd.uniforms.at("u_unlit").front() == 1.0f,
          "Tileset: glTF unlit material uploads the unlit shader switch");
    check(cmd.uniforms.count("u_materialFactors") &&
              std::abs(cmd.uniforms.at("u_materialFactors")[0] - 0.9f) <
                  1e-6f &&
              std::abs(cmd.uniforms.at("u_materialFactors")[1] - 0.1f) <
                  1e-6f,
          "Tileset: glTF unlit command keeps source material factors for diagnostics");
}

void testTilesetGltfEmissiveMaterialUploadsUniformsAndTextures() {
    DummyRenderDevice device;
    device.allowTextureCreation = true;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: glTF emissive material root tile is created");
    if (!root) return;

    root->gltfModel = makeTexturedTriangleGltfModel();
    GltfPrimitive& primitive = root->gltfModel->primitives[0];
    primitive.alphaMode = GltfAlphaMode::Opaque;
    primitive.emissiveFactor = {0.25f, 0.5f, 0.75f};
    GltfTextureBinding emissiveBinding;
    emissiveBinding.textureIndex = 0u;
    emissiveBinding.texCoord = 1;
    emissiveBinding.transform.offset = {0.125f, 0.25f};
    emissiveBinding.transform.scale = {0.5f, 0.75f};
    emissiveBinding.transform.rotation = 1.57079632679f;
    primitive.emissiveTexture = emissiveBinding;
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::ContentLoaded;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);

    check(commands.size() == 1,
          "Tileset: glTF emissive material emits one primitive draw command");
    if (commands.empty()) return;

    const RenderCommand& cmd = commands.front();
    check(cmd.uniforms.count("u_emissiveFactor") &&
              cmd.uniforms.at("u_emissiveFactor").size() == 3 &&
              std::abs(cmd.uniforms.at("u_emissiveFactor")[0] - 0.25f) <
                  1e-6f &&
              std::abs(cmd.uniforms.at("u_emissiveFactor")[1] - 0.5f) <
                  1e-6f &&
              std::abs(cmd.uniforms.at("u_emissiveFactor")[2] - 0.75f) <
                  1e-6f,
          "Tileset: glTF emissive material uploads emissiveFactor");
    check(cmd.uniforms.count("u_hasMaterialTextures") &&
              cmd.uniforms.at("u_hasMaterialTextures").size() == 4 &&
              cmd.uniforms.at("u_hasMaterialTextures")[3] == 1.0f,
          "Tileset: glTF emissive material reports emissive texture presence");
    check(cmd.textures.size() >= 5 &&
              cmd.textures[4] ==
                  root->gltfPrimitiveResources.front().emissiveTexture.texture,
          "Tileset: glTF emissive texture binds to material slot 4");
    check(cmd.uniforms.count("u_emissiveTexCoordSet") &&
              cmd.uniforms.at("u_emissiveTexCoordSet").front() == 1.0f,
          "Tileset: glTF emissive material uploads texture coordinate set");
    check(cmd.uniforms.count("u_emissiveTexOffsetScale") &&
              cmd.uniforms.at("u_emissiveTexOffsetScale").size() == 4 &&
              std::abs(cmd.uniforms.at("u_emissiveTexOffsetScale")[0] -
                       0.125f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_emissiveTexOffsetScale")[1] -
                       0.25f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_emissiveTexOffsetScale")[2] -
                       0.5f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_emissiveTexOffsetScale")[3] -
                       0.75f) < 1e-6f &&
              cmd.uniforms.count("u_emissiveTexRotationSinCos") &&
              std::abs(cmd.uniforms.at("u_emissiveTexRotationSinCos")[0] -
                       1.0f) < 1e-6f,
          "Tileset: glTF emissive texture uploads KHR_texture_transform uniforms");
}

void testTilesetGltfIorMaterialUploadsSpecularF0() {
    DummyRenderDevice device;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: glTF IOR material root tile is created");
    if (!root) return;

    root->gltfModel = makeTriangleGltfModel();
    root->gltfModel->primitives[0].dielectricSpecularF0 = 1.0f / 9.0f;
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::ContentLoaded;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);

    check(commands.size() == 1,
          "Tileset: glTF IOR material emits one primitive draw command");
    if (commands.empty()) return;

    const RenderCommand& cmd = commands.front();
    check(cmd.uniforms.count("u_dielectricSpecularF0") &&
              std::abs(cmd.uniforms.at("u_dielectricSpecularF0").front() -
                       1.0f / 9.0f) < 1e-6f,
          "Tileset: glTF IOR material uploads dielectric specular F0");
}

void testTilesetGltfAnisotropyMaterialUploadsUniformsAndTextures() {
    DummyRenderDevice device;
    device.allowTextureCreation = true;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: glTF anisotropy material root tile is created");
    if (!root) return;

    root->gltfModel = makeTexturedTriangleGltfModel();
    GltfPrimitive& primitive = root->gltfModel->primitives[0];
    primitive.anisotropyStrength = 0.8f;
    primitive.anisotropyRotation = 0.25f;
    GltfTextureBinding anisotropyBinding;
    anisotropyBinding.textureIndex = 0u;
    anisotropyBinding.texCoord = 1;
    anisotropyBinding.transform.offset = {0.125f, 0.25f};
    anisotropyBinding.transform.scale = {0.5f, 0.75f};
    anisotropyBinding.transform.rotation = 1.57079632679f;
    primitive.anisotropyTexture = anisotropyBinding;
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::ContentLoaded;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);

    check(commands.size() == 1,
          "Tileset: glTF anisotropy material emits one primitive draw command");
    if (commands.empty()) return;

    const RenderCommand& cmd = commands.front();
    check(cmd.uniforms.count("u_anisotropyFactors") &&
              cmd.uniforms.at("u_anisotropyFactors").size() == 2 &&
              std::abs(cmd.uniforms.at("u_anisotropyFactors")[0] - 0.8f) <
                  1e-6f &&
              std::abs(cmd.uniforms.at("u_anisotropyFactors")[1] - 0.25f) <
                  1e-6f,
          "Tileset: glTF anisotropy material uploads factors");
    check(cmd.uniforms.count("u_hasAnisotropyTexture") &&
              cmd.uniforms.at("u_hasAnisotropyTexture").front() == 1.0f,
          "Tileset: glTF anisotropy material reports texture presence");
    check(cmd.textures.size() >= 13 &&
              cmd.textures[12] ==
                  root->gltfPrimitiveResources.front()
                      .anisotropyTexture.texture,
          "Tileset: glTF anisotropy texture binds to material slot 12");
    check(cmd.uniforms.count("u_anisotropyTexCoordSet") &&
              cmd.uniforms.at("u_anisotropyTexCoordSet").front() == 1.0f,
          "Tileset: glTF anisotropy material uploads texture coordinate set");
    check(cmd.uniforms.count("u_anisotropyTexOffsetScale") &&
              cmd.uniforms.at("u_anisotropyTexOffsetScale").size() == 4 &&
              std::abs(cmd.uniforms.at("u_anisotropyTexOffsetScale")[0] -
                       0.125f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_anisotropyTexOffsetScale")[1] -
                       0.25f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_anisotropyTexOffsetScale")[2] -
                       0.5f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_anisotropyTexOffsetScale")[3] -
                       0.75f) < 1e-6f &&
              cmd.uniforms.count("u_anisotropyTexRotationSinCos") &&
              std::abs(cmd.uniforms.at("u_anisotropyTexRotationSinCos")[0] -
                       1.0f) < 1e-6f,
          "Tileset: glTF anisotropy texture uploads KHR_texture_transform uniforms");
}

void testTilesetGltfPbrSpecularGlossinessUploadsUniformsAndTextures() {
    DummyRenderDevice device;
    device.allowTextureCreation = true;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: glTF spec-gloss material root tile is created");
    if (!root) return;

    root->gltfModel = makeTexturedTriangleGltfModel();
    GltfPrimitive& primitive = root->gltfModel->primitives[0];
    primitive.specularGlossinessWorkflow = true;
    primitive.baseColorFactor = {0.2f, 0.3f, 0.4f, 0.5f};
    primitive.specularGlossinessSpecularFactor = {0.6f, 0.7f, 0.8f};
    primitive.specularGlossinessGlossinessFactor = 0.9f;
    GltfTextureBinding specGlossBinding;
    specGlossBinding.textureIndex = 0u;
    specGlossBinding.texCoord = 1;
    specGlossBinding.transform.offset = {0.125f, 0.25f};
    specGlossBinding.transform.scale = {0.5f, 0.75f};
    specGlossBinding.transform.rotation = 1.57079632679f;
    primitive.specularGlossinessTexture = specGlossBinding;
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::ContentLoaded;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);

    check(commands.size() == 1,
          "Tileset: glTF spec-gloss material emits one primitive draw command");
    if (commands.empty()) return;

    const RenderCommand& cmd = commands.front();
    check(cmd.uniforms.count("u_specularGlossinessWorkflow") &&
              cmd.uniforms.at("u_specularGlossinessWorkflow").front() ==
                  1.0f,
          "Tileset: glTF spec-gloss workflow flag is uploaded");
    check(cmd.uniforms.count("u_specularGlossinessFactor") &&
              cmd.uniforms.at("u_specularGlossinessFactor").size() == 4 &&
              std::abs(cmd.uniforms.at("u_specularGlossinessFactor")[0] -
                       0.6f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_specularGlossinessFactor")[1] -
                       0.7f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_specularGlossinessFactor")[2] -
                       0.8f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_specularGlossinessFactor")[3] -
                       0.9f) < 1e-6f,
          "Tileset: glTF spec-gloss factors are uploaded");
    check(cmd.uniforms.count("u_hasSpecularGlossinessTexture") &&
              cmd.uniforms.at("u_hasSpecularGlossinessTexture").front() ==
                  1.0f,
          "Tileset: glTF spec-gloss material reports texture presence");
    check(cmd.textures.size() >= 14 &&
              cmd.textures[13] ==
                  root->gltfPrimitiveResources.front()
                      .specularGlossinessTexture.texture,
          "Tileset: glTF spec-gloss texture binds to material slot 13");
    check(cmd.uniforms.count("u_specularGlossinessTexCoordSet") &&
              cmd.uniforms.at("u_specularGlossinessTexCoordSet").front() ==
                  1.0f,
          "Tileset: glTF spec-gloss material uploads texture coordinate set");
    check(cmd.uniforms.count("u_specularGlossinessTexOffsetScale") &&
              cmd.uniforms.at("u_specularGlossinessTexOffsetScale").size() ==
                  4 &&
              std::abs(cmd.uniforms.at("u_specularGlossinessTexOffsetScale")[0] -
                       0.125f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_specularGlossinessTexOffsetScale")[1] -
                       0.25f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_specularGlossinessTexOffsetScale")[2] -
                       0.5f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_specularGlossinessTexOffsetScale")[3] -
                       0.75f) < 1e-6f &&
              cmd.uniforms.count("u_specularGlossinessTexRotationSinCos") &&
              std::abs(
                  cmd.uniforms.at("u_specularGlossinessTexRotationSinCos")[0] -
                  1.0f) < 1e-6f,
          "Tileset: glTF spec-gloss texture uploads KHR_texture_transform uniforms");
}

void testTilesetGltfTransmissionMaterialUploadsUniformsAndTextures() {
    DummyRenderDevice device;
    device.allowTextureCreation = true;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: glTF transmission material root tile is created");
    if (!root) return;

    root->gltfModel = makeTexturedTriangleGltfModel();
    GltfPrimitive& primitive = root->gltfModel->primitives[0];
    primitive.alphaMode = GltfAlphaMode::Opaque;
    primitive.transmissionFactor = 0.65f;
    GltfTextureBinding transmissionBinding;
    transmissionBinding.textureIndex = 0u;
    transmissionBinding.texCoord = 1;
    transmissionBinding.transform.offset = {0.125f, 0.25f};
    transmissionBinding.transform.scale = {0.5f, 0.75f};
    transmissionBinding.transform.rotation = 1.57079632679f;
    primitive.transmissionTexture = transmissionBinding;
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::ContentLoaded;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);

    check(commands.size() == 1,
          "Tileset: glTF transmission material emits one primitive draw command");
    if (commands.empty()) return;

    const RenderCommand& cmd = commands.front();
    check(cmd.blend && !cmd.depthWrite,
          "Tileset: glTF transmission material becomes a translucent command");
    check(cmd.uniforms.count("u_transmissionFactor") &&
              std::abs(cmd.uniforms.at("u_transmissionFactor").front() -
                       0.65f) < 1e-6f,
          "Tileset: glTF transmission material uploads transmissionFactor");
    check(cmd.uniforms.count("u_hasTransmissionTexture") &&
              cmd.uniforms.at("u_hasTransmissionTexture").front() == 1.0f,
          "Tileset: glTF transmission material reports texture presence");
    check(cmd.textures.size() >= 15 &&
              cmd.textures[14] ==
                  root->gltfPrimitiveResources.front()
                      .transmissionTexture.texture,
          "Tileset: glTF transmission texture binds to material slot 14");
    check(cmd.uniforms.count("u_transmissionTexCoordSet") &&
              cmd.uniforms.at("u_transmissionTexCoordSet").front() == 1.0f,
          "Tileset: glTF transmission material uploads texture coordinate set");
    check(cmd.uniforms.count("u_transmissionTexOffsetScale") &&
              cmd.uniforms.at("u_transmissionTexOffsetScale").size() == 4 &&
              std::abs(cmd.uniforms.at("u_transmissionTexOffsetScale")[0] -
                       0.125f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_transmissionTexOffsetScale")[1] -
                       0.25f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_transmissionTexOffsetScale")[2] -
                       0.5f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_transmissionTexOffsetScale")[3] -
                       0.75f) < 1e-6f &&
              cmd.uniforms.count("u_transmissionTexRotationSinCos") &&
              std::abs(cmd.uniforms.at("u_transmissionTexRotationSinCos")[0] -
                       1.0f) < 1e-6f,
          "Tileset: glTF transmission texture uploads KHR_texture_transform uniforms");
}

void testTilesetGltfSpecularMaterialUploadsUniformsAndTextures() {
    DummyRenderDevice device;
    device.allowTextureCreation = true;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: glTF specular material root tile is created");
    if (!root) return;

    root->gltfModel = makeTexturedTriangleGltfModel();
    GltfPrimitive& primitive = root->gltfModel->primitives[0];
    primitive.specularFactor = 0.25f;
    primitive.specularColorFactor = {2.0f, 0.5f, 0.25f};
    GltfTextureBinding specularBinding;
    specularBinding.textureIndex = 0u;
    specularBinding.texCoord = 1;
    specularBinding.transform.offset = {0.125f, 0.25f};
    specularBinding.transform.scale = {0.5f, 0.75f};
    specularBinding.transform.rotation = 1.57079632679f;
    primitive.specularTexture = specularBinding;
    GltfTextureBinding specularColorBinding;
    specularColorBinding.textureIndex = 0u;
    specularColorBinding.texCoord = 0;
    primitive.specularColorTexture = specularColorBinding;
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::ContentLoaded;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);

    check(commands.size() == 1,
          "Tileset: glTF specular material emits one primitive draw command");
    if (commands.empty()) return;

    const RenderCommand& cmd = commands.front();
    check(cmd.uniforms.count("u_specularFactor") &&
              std::abs(cmd.uniforms.at("u_specularFactor").front() - 0.25f) <
                  1e-6f,
          "Tileset: glTF specular material uploads specularFactor");
    check(cmd.uniforms.count("u_specularColorFactor") &&
              cmd.uniforms.at("u_specularColorFactor").size() == 3 &&
              std::abs(cmd.uniforms.at("u_specularColorFactor")[0] - 2.0f) <
                  1e-6f &&
              std::abs(cmd.uniforms.at("u_specularColorFactor")[1] - 0.5f) <
                  1e-6f &&
              std::abs(cmd.uniforms.at("u_specularColorFactor")[2] - 0.25f) <
                  1e-6f,
          "Tileset: glTF specular material uploads specularColorFactor");
    check(cmd.uniforms.count("u_hasSpecularTextures") &&
              cmd.uniforms.at("u_hasSpecularTextures").size() == 2 &&
              cmd.uniforms.at("u_hasSpecularTextures")[0] == 1.0f &&
              cmd.uniforms.at("u_hasSpecularTextures")[1] == 1.0f,
          "Tileset: glTF specular material reports both specular textures");
    check(cmd.textures.size() >= 7 &&
              cmd.textures[5] ==
                  root->gltfPrimitiveResources.front().specularTexture.texture &&
              cmd.textures[6] ==
                  root->gltfPrimitiveResources.front()
                      .specularColorTexture.texture,
          "Tileset: glTF specular textures bind to material slots 5 and 6");
    check(cmd.uniforms.count("u_specularTexCoordSets") &&
              cmd.uniforms.at("u_specularTexCoordSets").size() == 2 &&
              cmd.uniforms.at("u_specularTexCoordSets")[0] == 1.0f &&
              cmd.uniforms.at("u_specularTexCoordSets")[1] == 0.0f,
          "Tileset: glTF specular material uploads texture coordinate sets");
    check(cmd.uniforms.count("u_specularTexOffsetScale") &&
              cmd.uniforms.at("u_specularTexOffsetScale").size() == 4 &&
              std::abs(cmd.uniforms.at("u_specularTexOffsetScale")[0] -
                       0.125f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_specularTexOffsetScale")[1] -
                       0.25f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_specularTexOffsetScale")[2] -
                       0.5f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_specularTexOffsetScale")[3] -
                       0.75f) < 1e-6f &&
              cmd.uniforms.count("u_specularTexRotationSinCos") &&
              std::abs(cmd.uniforms.at("u_specularTexRotationSinCos")[0] -
                       1.0f) < 1e-6f,
          "Tileset: glTF specular texture uploads KHR_texture_transform uniforms");
}

void testTilesetGltfClearcoatMaterialUploadsUniformsAndTextures() {
    DummyRenderDevice device;
    device.allowTextureCreation = true;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: glTF clearcoat material root tile is created");
    if (!root) return;

    root->gltfModel = makeTexturedTriangleGltfModel();
    GltfPrimitive& primitive = root->gltfModel->primitives[0];
    primitive.clearcoatFactor = 0.75f;
    primitive.clearcoatRoughnessFactor = 0.25f;
    primitive.clearcoatNormalTextureScale = 0.6f;
    GltfTextureBinding clearcoatBinding;
    clearcoatBinding.textureIndex = 0u;
    clearcoatBinding.texCoord = 1;
    clearcoatBinding.transform.offset = {0.125f, 0.25f};
    clearcoatBinding.transform.scale = {0.5f, 0.75f};
    clearcoatBinding.transform.rotation = 1.57079632679f;
    primitive.clearcoatTexture = clearcoatBinding;
    GltfTextureBinding clearcoatRoughnessBinding;
    clearcoatRoughnessBinding.textureIndex = 0u;
    clearcoatRoughnessBinding.texCoord = 0;
    primitive.clearcoatRoughnessTexture = clearcoatRoughnessBinding;
    GltfTextureBinding clearcoatNormalBinding;
    clearcoatNormalBinding.textureIndex = 0u;
    clearcoatNormalBinding.texCoord = 1;
    primitive.clearcoatNormalTexture = clearcoatNormalBinding;
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::ContentLoaded;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);

    check(commands.size() == 1,
          "Tileset: glTF clearcoat material emits one primitive draw command");
    if (commands.empty()) return;

    const RenderCommand& cmd = commands.front();
    check(cmd.uniforms.count("u_clearcoatFactors") &&
              cmd.uniforms.at("u_clearcoatFactors").size() == 3 &&
              std::abs(cmd.uniforms.at("u_clearcoatFactors")[0] - 0.75f) <
                  1e-6f &&
              std::abs(cmd.uniforms.at("u_clearcoatFactors")[1] - 0.25f) <
                  1e-6f &&
              std::abs(cmd.uniforms.at("u_clearcoatFactors")[2] - 0.6f) <
                  1e-6f,
          "Tileset: glTF clearcoat material uploads clearcoat factors");
    check(cmd.uniforms.count("u_hasClearcoatTextures") &&
              cmd.uniforms.at("u_hasClearcoatTextures").size() == 3 &&
              cmd.uniforms.at("u_hasClearcoatTextures")[0] == 1.0f &&
              cmd.uniforms.at("u_hasClearcoatTextures")[1] == 1.0f &&
              cmd.uniforms.at("u_hasClearcoatTextures")[2] == 1.0f,
          "Tileset: glTF clearcoat material reports all clearcoat textures");
    check(cmd.textures.size() >= 10 &&
              cmd.textures[7] ==
                  root->gltfPrimitiveResources.front().clearcoatTexture.texture &&
              cmd.textures[8] ==
                  root->gltfPrimitiveResources.front()
                      .clearcoatRoughnessTexture.texture &&
              cmd.textures[9] ==
                  root->gltfPrimitiveResources.front()
                      .clearcoatNormalTexture.texture,
          "Tileset: glTF clearcoat textures bind to material slots 7, 8, and 9");
    check(cmd.uniforms.count("u_clearcoatTexCoordSets") &&
              cmd.uniforms.at("u_clearcoatTexCoordSets").size() == 3 &&
              cmd.uniforms.at("u_clearcoatTexCoordSets")[0] == 1.0f &&
              cmd.uniforms.at("u_clearcoatTexCoordSets")[1] == 0.0f &&
              cmd.uniforms.at("u_clearcoatTexCoordSets")[2] == 1.0f,
          "Tileset: glTF clearcoat material uploads texture coordinate sets");
    check(cmd.uniforms.count("u_clearcoatTexOffsetScale") &&
              cmd.uniforms.at("u_clearcoatTexOffsetScale").size() == 4 &&
              std::abs(cmd.uniforms.at("u_clearcoatTexOffsetScale")[0] -
                       0.125f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_clearcoatTexOffsetScale")[1] -
                       0.25f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_clearcoatTexOffsetScale")[2] -
                       0.5f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_clearcoatTexOffsetScale")[3] -
                       0.75f) < 1e-6f &&
              cmd.uniforms.count("u_clearcoatTexRotationSinCos") &&
              std::abs(cmd.uniforms.at("u_clearcoatTexRotationSinCos")[0] -
                       1.0f) < 1e-6f,
          "Tileset: glTF clearcoat texture uploads KHR_texture_transform uniforms");
}

void testTilesetGltfSheenMaterialUploadsUniformsAndTextures() {
    DummyRenderDevice device;
    device.allowTextureCreation = true;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: glTF sheen material root tile is created");
    if (!root) return;

    root->gltfModel = makeTexturedTriangleGltfModel();
    GltfPrimitive& primitive = root->gltfModel->primitives[0];
    primitive.sheenColorFactor = {0.2f, 0.4f, 0.6f};
    primitive.sheenRoughnessFactor = 0.35f;
    GltfTextureBinding sheenColorBinding;
    sheenColorBinding.textureIndex = 0u;
    sheenColorBinding.texCoord = 1;
    sheenColorBinding.transform.offset = {0.125f, 0.25f};
    sheenColorBinding.transform.scale = {0.5f, 0.75f};
    sheenColorBinding.transform.rotation = 1.57079632679f;
    primitive.sheenColorTexture = sheenColorBinding;
    GltfTextureBinding sheenRoughnessBinding;
    sheenRoughnessBinding.textureIndex = 0u;
    sheenRoughnessBinding.texCoord = 0;
    primitive.sheenRoughnessTexture = sheenRoughnessBinding;
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::ContentLoaded;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);

    check(commands.size() == 1,
          "Tileset: glTF sheen material emits one primitive draw command");
    if (commands.empty()) return;

    const RenderCommand& cmd = commands.front();
    check(cmd.uniforms.count("u_sheenColorFactor") &&
              cmd.uniforms.at("u_sheenColorFactor").size() == 3 &&
              std::abs(cmd.uniforms.at("u_sheenColorFactor")[0] - 0.2f) <
                  1e-6f &&
              std::abs(cmd.uniforms.at("u_sheenColorFactor")[1] - 0.4f) <
                  1e-6f &&
              std::abs(cmd.uniforms.at("u_sheenColorFactor")[2] - 0.6f) <
                  1e-6f,
          "Tileset: glTF sheen material uploads sheenColorFactor");
    check(cmd.uniforms.count("u_sheenRoughnessFactor") &&
              std::abs(
                  cmd.uniforms.at("u_sheenRoughnessFactor").front() -
                  0.35f) < 1e-6f,
          "Tileset: glTF sheen material uploads sheenRoughnessFactor");
    check(cmd.uniforms.count("u_hasSheenTextures") &&
              cmd.uniforms.at("u_hasSheenTextures").size() == 2 &&
              cmd.uniforms.at("u_hasSheenTextures")[0] == 1.0f &&
              cmd.uniforms.at("u_hasSheenTextures")[1] == 1.0f,
          "Tileset: glTF sheen material reports both sheen textures");
    check(cmd.textures.size() >= 12 &&
              cmd.textures[10] ==
                  root->gltfPrimitiveResources.front()
                      .sheenColorTexture.texture &&
              cmd.textures[11] ==
                  root->gltfPrimitiveResources.front()
                      .sheenRoughnessTexture.texture,
          "Tileset: glTF sheen textures bind to material slots 10 and 11");
    check(cmd.uniforms.count("u_sheenTexCoordSets") &&
              cmd.uniforms.at("u_sheenTexCoordSets").size() == 2 &&
              cmd.uniforms.at("u_sheenTexCoordSets")[0] == 1.0f &&
              cmd.uniforms.at("u_sheenTexCoordSets")[1] == 0.0f,
          "Tileset: glTF sheen material uploads texture coordinate sets");
    check(cmd.uniforms.count("u_sheenColorTexOffsetScale") &&
              cmd.uniforms.at("u_sheenColorTexOffsetScale").size() == 4 &&
              std::abs(cmd.uniforms.at("u_sheenColorTexOffsetScale")[0] -
                       0.125f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_sheenColorTexOffsetScale")[1] -
                       0.25f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_sheenColorTexOffsetScale")[2] -
                       0.5f) < 1e-6f &&
              std::abs(cmd.uniforms.at("u_sheenColorTexOffsetScale")[3] -
                       0.75f) < 1e-6f &&
              cmd.uniforms.count("u_sheenColorTexRotationSinCos") &&
              std::abs(cmd.uniforms.at("u_sheenColorTexRotationSinCos")[0] -
                       1.0f) < 1e-6f,
          "Tileset: glTF sheen color texture uploads KHR_texture_transform uniforms");
}

void testTilesetGltfPointAndLineModesReachDrawCommands() {
    DummyRenderDevice device;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: glTF point/line mode root tile is created");
    if (!root) return;

    root->gltfModel = makeTriangleGltfModel();
    GltfPrimitive points = root->gltfModel->primitives[0];
    points.primitiveMode = GltfPrimitiveMode::Points;
    points.indices = {0, 1, 2};
    GltfPrimitive lines = root->gltfModel->primitives[0];
    lines.primitiveMode = GltfPrimitiveMode::Lines;
    lines.indices = {0, 1, 1, 2};
    root->gltfModel->primitives.clear();
    root->gltfModel->primitives.push_back(std::move(points));
    root->gltfModel->primitives.push_back(std::move(lines));
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::ContentLoaded;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);

    check(commands.size() == 2,
          "Tileset: glTF point and line primitives emit separate draw commands");
    if (commands.size() < 2) return;
    check(commands[0].primitive == RenderCommand::PrimitiveType::Points &&
              commands[1].primitive == RenderCommand::PrimitiveType::Lines,
          "Tileset: glTF primitive modes reach renderer draw command topology");
}

void testTilesetGltfDoubleSidedDisablesCullOnly() {
    DummyRenderDevice device;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: glTF doubleSided material root tile is created");
    if (!root) return;

    root->gltfModel = makeTriangleGltfModel();
    root->gltfModel->primitives[0].doubleSided = true;
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::ContentLoaded;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);

    check(commands.size() == 1,
          "Tileset: glTF doubleSided material emits one primitive draw command");
    if (commands.empty()) return;

    const RenderCommand& cmd = commands.front();
    check(!cmd.cullFace,
          "Tileset: glTF doubleSided material disables back-face culling");
    check(!cmd.blend && cmd.depthWrite,
          "Tileset: glTF doubleSided material stays opaque and depth-writing");
}

void testTilesetGltfOpaqueInstancesUseGpuInstancing() {
    DummyRenderDevice device;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: glTF opaque instancing root tile is created");
    if (!root) return;

    root->gltfModel = makeTriangleGltfModel();
    GltfPrimitive& primitive = root->gltfModel->primitives[0];
    GltfInstance firstInstance;
    firstInstance.transform = Mat4::translation(Vec3(1.0, 2.0, 3.0));
    GltfInstance secondInstance;
    secondInstance.transform = Mat4::translation(Vec3(4.0, 5.0, 6.0));
    primitive.instances = {firstInstance, secondInstance};
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::ContentLoaded;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);

    check(commands.size() == 1,
          "Tileset: glTF opaque instances stay in one draw command");
    if (commands.empty()) return;

    const RenderCommand& cmd = commands.front();
    check(cmd.kind == RenderCommandKind::GltfPrimitiveInstanced &&
              cmd.instanceCount == 2 &&
              cmd.instanceBuffer != nullptr &&
              !cmd.blend &&
              cmd.depthWrite,
          "Tileset: glTF opaque instances use GPU instanced draw command");
}

void testTilesetGltfBlendInstancesSplitForSorting() {
    DummyRenderDevice device;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: glTF BLEND instancing root tile is created");
    if (!root) return;

    root->gltfModel = makeTriangleGltfModel();
    GltfPrimitive& primitive = root->gltfModel->primitives[0];
    primitive.alphaMode = GltfAlphaMode::Blend;
    GltfInstance nearInstance;
    nearInstance.transform = Mat4::translation(Vec3(0.0, 0.0, 5.0));
    GltfInstance farInstance;
    farInstance.transform = Mat4::translation(Vec3(0.0, 0.0, 25.0));
    primitive.instances = {nearInstance, farInstance};
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::ContentLoaded;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);

    check(commands.size() == 2 &&
              root->gltfPrimitiveResources.size() == 2u,
          "Tileset: glTF BLEND instances become separate sortable draw commands");
    if (commands.size() < 2) return;

    bool splitCommands = true;
    for (const RenderCommand& cmd : commands) {
        splitCommands = splitCommands &&
            cmd.kind == RenderCommandKind::GltfPrimitive &&
            cmd.instanceCount == 0 &&
            cmd.instanceBuffer == nullptr &&
            cmd.blend &&
            !cmd.depthWrite &&
            cmd.hasWorldSortCenter;
    }
    check(splitCommands,
          "Tileset: split BLEND instance commands are non-instanced translucent primitives");
    check(std::abs(commands[0].worldSortCenter[2] - 5.0) < 1e-9 &&
              std::abs(commands[1].worldSortCenter[2] - 25.0) < 1e-9,
          "Tileset: split BLEND instance commands carry per-instance sort centers");

    const auto* firstVertices =
        dynamic_cast<const DummyBuffer*>(commands[0].vertexBuffer);
    const auto* secondVertices =
        dynamic_cast<const DummyBuffer*>(commands[1].vertexBuffer);
    bool verticesBakedPerInstance = false;
    if (firstVertices && secondVertices &&
        firstVertices->bytes().size() >= 3u * sizeof(float) &&
        secondVertices->bytes().size() >= 3u * sizeof(float)) {
        std::array<float, 3> first{};
        std::array<float, 3> second{};
        std::memcpy(first.data(), firstVertices->bytes().data(), sizeof(first));
        std::memcpy(second.data(), secondVertices->bytes().data(), sizeof(second));
        verticesBakedPerInstance =
            std::abs(first[2] + 10.0f) < 1e-6f &&
            std::abs(second[2] - 10.0f) < 1e-6f;
    }
    check(verticesBakedPerInstance,
          "Tileset: split BLEND instance vertices are baked relative to shared local origin");
}

void testTilesetGltfTransmissionInstancesSplitForSorting() {
    DummyRenderDevice device;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: glTF transmission instancing root tile is created");
    if (!root) return;

    root->gltfModel = makeTriangleGltfModel();
    GltfPrimitive& primitive = root->gltfModel->primitives[0];
    primitive.transmissionFactor = 0.5f;
    GltfInstance nearInstance;
    nearInstance.transform = Mat4::translation(Vec3(0.0, 0.0, 5.0));
    GltfInstance farInstance;
    farInstance.transform = Mat4::translation(Vec3(0.0, 0.0, 25.0));
    primitive.instances = {nearInstance, farInstance};
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::ContentLoaded;

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);

    check(commands.size() == 2 &&
              root->gltfPrimitiveResources.size() == 2u,
          "Tileset: glTF transmission instances become separate sortable draw commands");
    if (commands.size() < 2) return;

    bool splitCommands = true;
    for (const RenderCommand& cmd : commands) {
        splitCommands = splitCommands &&
            cmd.kind == RenderCommandKind::GltfPrimitive &&
            cmd.instanceCount == 0 &&
            cmd.instanceBuffer == nullptr &&
            cmd.blend &&
            !cmd.depthWrite &&
            cmd.hasWorldSortCenter &&
            cmd.uniforms.count("u_transmissionFactor") &&
            std::abs(cmd.uniforms.at("u_transmissionFactor").front() -
                     0.5f) < 1e-6f;
    }
    check(splitCommands,
          "Tileset: split transmission instance commands are non-instanced translucent primitives");
    check(std::abs(commands[0].worldSortCenter[2] - 5.0) < 1e-9 &&
              std::abs(commands[1].worldSortCenter[2] - 25.0) < 1e-9,
          "Tileset: split transmission instance commands carry per-instance sort centers");
}

void testTilesetContentProviderLoadsGltfRenderContent() {
    DummyRenderDevice device;
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    auto contentProvider = std::make_unique<SingleGltfContentProvider>(
        rootKey,
        makeTriangleGlbBytes(),
        "triangle GLB fixture");
    contentProvider->setContentTransform(
        Mat4::translation(Vec3(100.0, 200.0, 300.0)));
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTestAccess::requestMissingTile(tileset, rootKey);

    TilesetTile* root = nullptr;
    for (int i = 0; i < 100; ++i) {
        TilesetTestAccess::processPendingUploads(tileset);
        root = TilesetTestAccess::findTile(tileset, rootKey);
        if (root &&
            root->loadState == TileLoadState::Done &&
            root->contentKind == TileContentKind::Render &&
            root->gltfModel) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    check(root != nullptr,
          "Tileset: content provider creates the requested root tile");
    if (!root) return;

    check(root->loadState == TileLoadState::Done &&
              root->contentKind == TileContentKind::Render &&
              root->gltfModel != nullptr,
          "Tileset: content provider GLB enters render content done state");
    check(root->mesh == nullptr && root->heightmap == nullptr &&
              root->gpuVertexBuffer == nullptr,
          "Tileset: content provider GLB does not populate terrain content fields");
    check(root->gltfPrimitiveResources.size() == 1,
          "Tileset: content provider GLB prepares primitive render resources");
    check(std::abs(root->localOrigin.x() - (100.0 + 1.0 / 3.0)) < 1e-12 &&
              std::abs(root->localOrigin.y() - (200.0 + 1.0 / 3.0)) < 1e-12 &&
              std::abs(root->localOrigin.z() - 300.0) < 1e-12,
          "Tileset: content provider transform contributes to glTF RTC origin");
    if (!root->gltfPrimitiveResources.empty()) {
        const auto* vertexBuffer = dynamic_cast<const DummyBuffer*>(
            root->gltfPrimitiveResources.front().vertexBuffer.get());
        if (vertexBuffer && vertexBuffer->bytes().size() >= 12) {
            float firstPos[3] = {};
            std::memcpy(firstPos,
                        vertexBuffer->bytes().data(),
                        sizeof(firstPos));
            check(std::abs(firstPos[0] + 1.0f / 3.0f) < 1e-6f &&
                      std::abs(firstPos[1] + 1.0f / 3.0f) < 1e-6f &&
                      std::abs(firstPos[2]) < 1e-6f,
                  "Tileset: glTF GPU vertices are relative to transformed RTC origin");
        } else {
            check(false,
                  "Tileset: glTF test buffer captures uploaded vertex data");
        }
    }

    const TilesetLoadDiagnostics diag = tileset.loadDiagnostics();
    check(diag.pendingContentTotal() == 0,
          "Tileset: content provider GLB drains content pending lifecycle queues");

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);
    check(commands.size() == 1 &&
              commands.front().kind == RenderCommandKind::GltfPrimitive,
          "Tileset: loaded content provider GLB renders through GltfPrimitive command");
}

void testTilesetContentProviderLoadsNativeGpuInstancingGltf() {
    DummyRenderDevice device;
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    auto contentProvider = std::make_unique<SingleGltfContentProvider>(
        rootKey,
        makeNativeGpuInstancedTriangleGlbBytes(),
        "native EXT_mesh_gpu_instancing GLB fixture");
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTestAccess::requestMissingTile(tileset, rootKey);

    TilesetTile* root = nullptr;
    for (int i = 0; i < 100; ++i) {
        TilesetTestAccess::processPendingUploads(tileset);
        root = TilesetTestAccess::findTile(tileset, rootKey);
        if (root &&
            root->loadState == TileLoadState::Done &&
            root->contentKind == TileContentKind::Render &&
            root->gltfModel) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    check(root != nullptr,
          "Tileset: native glTF instancing provider creates the requested tile");
    if (!root) return;

    check(root->loadState == TileLoadState::Done &&
              root->contentKind == TileContentKind::Render &&
              root->gltfModel != nullptr,
          "Tileset: native glTF instancing content reaches render state");
    check(root->gltfModel &&
              root->gltfModel->primitives.size() == 1 &&
              root->gltfModel->primitives.front().instances.size() == 2,
          "Tileset: native EXT_mesh_gpu_instancing parses two instances");
    check(root->gltfPrimitiveResources.size() == 1 &&
              root->gltfPrimitiveResources.front().instanceCount == 2 &&
              root->gltfPrimitiveResources.front().instanceBuffer != nullptr,
          "Tileset: native EXT_mesh_gpu_instancing prepares one GPU instance buffer");
    if (!root->gltfPrimitiveResources.empty()) {
        const auto* instanceBuffer = dynamic_cast<const DummyBuffer*>(
            root->gltfPrimitiveResources.front().instanceBuffer.get());
        check(instanceBuffer &&
                  instanceBuffer->bytes().size() ==
                      2u * (16u + 9u) * sizeof(float),
              "Tileset: native EXT_mesh_gpu_instancing uploads instance matrices");
    }

    Renderer renderer(nullptr);
    RenderCommandList commands;
    TilesetTestAccess::buildTileDrawCommand(
        tileset,
        renderer,
        *root,
        commands,
        1.0f);
    check(commands.size() == 1 &&
              commands.front().kind ==
                  RenderCommandKind::GltfPrimitiveInstanced &&
              commands.front().instanceCount == 2 &&
              commands.front().instanceBuffer != nullptr &&
              !commands.front().blend &&
              commands.front().depthWrite,
          "Tileset: native EXT_mesh_gpu_instancing renders as opaque GPU instancing");
}

void testTilesetJsonProviderLoadsExplicitGltfTile() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth-md-tileset-json-explicit";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "models");
    const ExternalGltfBytes externalGltf = makeTriangleExternalGltfBytes();
    writeBytes(root / "models" / "triangle.gltf", externalGltf.jsonBytes);
    writeBytes(root / "models" / "triangle.bin", externalGltf.binBytes);

    const std::string tilesetJson = R"json({
      "asset": {"version": "1.1"},
      "geometricError": 100,
      "root": {
        "boundingVolume": {"region": [-0.01, -0.01, 0.01, 0.01, 0, 100]},
        "geometricError": 64,
        "refine": "ADD",
        "transform": [1,0,0,0, 0,1,0,0, 0,0,1,0, 100,200,300,1],
        "content": {"uri": "models/triangle.gltf"}
      }
    })json";
    auto provider = std::make_unique<TilesetJsonContentProvider>(
        "file://" + (root / "tileset.json").generic_string(),
        std::vector<uint8_t>(tilesetJson.begin(), tilesetJson.end()),
        "explicit tileset fixture");
    TilesetJsonContentProvider* rawProvider = provider.get();
    check(rawProvider->valid(),
          "TilesetJsonContentProvider: explicit tileset parses");
    const std::vector<TileKey> roots = rawProvider->rootTiles();
    check(roots.size() == 1,
          "TilesetJsonContentProvider: initial tileset creates one native wrapper root");
    if (roots.empty()) return;
    const std::vector<TileKey> rootChildren =
        rawProvider->childTiles(roots.front());
    check(rootChildren.size() == 1,
          "TilesetJsonContentProvider: wrapper root owns the parsed root tile");
    if (rootChildren.empty()) return;

    DummyRenderDevice device;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{},
        std::move(provider));

    TilesetTestAccess::requestMissingTile(tileset, roots.front());
    TilesetTestAccess::processPendingUploads(tileset);
    TilesetTile* wrapper = TilesetTestAccess::findTile(tileset, roots.front());
    check(wrapper &&
              wrapper->loadState == TileLoadState::Done &&
              wrapper->contentKind == TileContentKind::External &&
              wrapper->unconditionallyRefine &&
              !wrapper->children.empty(),
          "Tileset: tileset.json wrapper loads as external unconditional-refine content");

    const TileKey contentKey = rootChildren.front();
    TilesetTestAccess::requestMissingTile(tileset, contentKey);
    TilesetTile* contentTile = nullptr;
    for (int i = 0; i < 100; ++i) {
        TilesetTestAccess::processPendingUploads(tileset);
        contentTile = TilesetTestAccess::findTile(tileset, contentKey);
        if (contentTile &&
            contentTile->loadState == TileLoadState::Done &&
            contentTile->contentKind == TileContentKind::Render &&
            contentTile->gltfModel) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    check(contentTile &&
              contentTile->refine == TileRefine::Add &&
              contentTile->boundingVolume.has_value(),
          "Tileset: JSON tile metadata applies refine and explicit bounding volume");
    check(contentTile &&
              contentTile->gltfModel &&
              std::abs((contentTile->gltfContentTransform *
                        contentTile->gltfModel->primitives[0].vertices[0]
                            .positionEcef).x() - 100.0) < 1e-12 &&
              std::abs((contentTile->gltfContentTransform *
                        contentTile->gltfModel->primitives[0].vertices[0]
                            .positionEcef).y() - 200.0) < 1e-12 &&
              std::abs((contentTile->gltfContentTransform *
                        contentTile->gltfModel->primitives[0].vertices[0]
                            .positionEcef).z() - 300.0) < 1e-12,
          "Tileset: JSON tile transform composes into glTF content");
    check(contentTile &&
              contentTile->gltfModel &&
              std::abs((contentTile->gltfContentTransform *
                        contentTile->gltfModel->primitives[0].vertices[2]
                            .positionEcef).x() - 100.0) < 1e-12 &&
              std::abs((contentTile->gltfContentTransform *
                        contentTile->gltfModel->primitives[0].vertices[2]
                            .positionEcef).y() - 200.0) < 1e-12 &&
              std::abs((contentTile->gltfContentTransform *
                        contentTile->gltfModel->primitives[0].vertices[2]
                            .positionEcef).z() - 301.0) < 1e-12,
          "Tileset: asset.gltfUpAxis default Y converts glTF Y-up to Z-up");

    std::filesystem::remove_all(root);
}

void testTilesetJsonGltfUpAxisZKeepsZUpContent() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth-md-tileset-json-gltf-z-up";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "models");
    const ExternalGltfBytes externalGltf = makeTriangleExternalGltfBytes();
    writeBytes(root / "models" / "triangle.gltf", externalGltf.jsonBytes);
    writeBytes(root / "models" / "triangle.bin", externalGltf.binBytes);

    const std::string tilesetJson = R"json({
      "asset": {"version": "1.1", "gltfUpAxis": "Z"},
      "geometricError": 100,
      "root": {
        "boundingVolume": {"region": [-0.01, -0.01, 0.01, 0.01, 0, 100]},
        "geometricError": 64,
        "transform": [1,0,0,0, 0,1,0,0, 0,0,1,0, 100,200,300,1],
        "content": {"uri": "models/triangle.gltf"}
      }
    })json";
    auto provider = std::make_unique<TilesetJsonContentProvider>(
        "file://" + (root / "tileset.json").generic_string(),
        std::vector<uint8_t>(tilesetJson.begin(), tilesetJson.end()),
        "z-up tileset fixture");
    TilesetJsonContentProvider* rawProvider = provider.get();
    check(rawProvider->valid(),
          "TilesetJsonContentProvider: gltfUpAxis Z tileset parses");
    const std::vector<TileKey> roots = rawProvider->rootTiles();
    if (roots.empty()) {
        check(false,
              "TilesetJsonContentProvider: gltfUpAxis Z root exists");
        std::filesystem::remove_all(root);
        return;
    }
    const std::vector<TileKey> rootChildren =
        rawProvider->childTiles(roots.front());
    if (rootChildren.empty()) {
        check(false,
              "TilesetJsonContentProvider: gltfUpAxis Z content child exists");
        std::filesystem::remove_all(root);
        return;
    }

    DummyRenderDevice device;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{},
        std::move(provider));

    TilesetTestAccess::requestMissingTile(tileset, roots.front());
    TilesetTestAccess::processPendingUploads(tileset);
    const TileKey contentKey = rootChildren.front();
    TilesetTestAccess::requestMissingTile(tileset, contentKey);
    TilesetTile* contentTile = nullptr;
    for (int i = 0; i < 100; ++i) {
        TilesetTestAccess::processPendingUploads(tileset);
        contentTile = TilesetTestAccess::findTile(tileset, contentKey);
        if (contentTile &&
            contentTile->loadState == TileLoadState::Done &&
            contentTile->contentKind == TileContentKind::Render &&
            contentTile->gltfModel) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    check(contentTile &&
              contentTile->gltfModel &&
              std::abs((contentTile->gltfContentTransform *
                        contentTile->gltfModel->primitives[0].vertices[2]
                            .positionEcef).x() - 100.0) < 1e-12 &&
              std::abs((contentTile->gltfContentTransform *
                        contentTile->gltfModel->primitives[0].vertices[2]
                            .positionEcef).y() - 201.0) < 1e-12 &&
              std::abs((contentTile->gltfContentTransform *
                        contentTile->gltfModel->primitives[0].vertices[2]
                            .positionEcef).z() - 300.0) < 1e-12,
          "Tileset: asset.gltfUpAxis Z preserves already-Z-up glTF content");

    std::filesystem::remove_all(root);
}

void testTilesetJsonI3dmDefaultUpAxisKeepsInstancePositionsTileLocal() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth-md-tileset-json-i3dm-default-up-axis";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "models");
    writeBytes(
        root / "models" / "instances.i3dm",
        makeI3dmBytes(
            makeTriangleGlbBytes(),
            {Vec3(0.0, 0.0, 0.0), Vec3(0.0, 10.0, 0.0)}));

    const std::string tilesetJson = R"json({
      "asset": {"version": "1.1"},
      "geometricError": 100,
      "root": {
        "boundingVolume": {"region": [-0.01, -0.01, 0.01, 0.01, 0, 100]},
        "geometricError": 64,
        "content": {"uri": "models/instances.i3dm"}
      }
    })json";
    auto provider = std::make_unique<TilesetJsonContentProvider>(
        "file://" + (root / "tileset.json").generic_string(),
        std::vector<uint8_t>(tilesetJson.begin(), tilesetJson.end()),
        "i3dm default-up-axis fixture");
    TilesetJsonContentProvider* rawProvider = provider.get();
    check(rawProvider->valid(),
          "TilesetJsonContentProvider: default-up-axis i3dm tileset parses");
    const std::vector<TileKey> roots = rawProvider->rootTiles();
    if (roots.empty()) {
        check(false,
              "TilesetJsonContentProvider: default-up-axis i3dm root exists");
        std::filesystem::remove_all(root);
        return;
    }
    const std::vector<TileKey> rootChildren =
        rawProvider->childTiles(roots.front());
    if (rootChildren.empty()) {
        check(false,
              "TilesetJsonContentProvider: default-up-axis i3dm content child exists");
        std::filesystem::remove_all(root);
        return;
    }

    DummyRenderDevice device;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{},
        std::move(provider));

    TilesetTestAccess::requestMissingTile(tileset, roots.front());
    TilesetTestAccess::processPendingUploads(tileset);
    const TileKey contentKey = rootChildren.front();
    TilesetTestAccess::requestMissingTile(tileset, contentKey);
    TilesetTile* contentTile = nullptr;
    for (int i = 0; i < 100; ++i) {
        TilesetTestAccess::processPendingUploads(tileset);
        contentTile = TilesetTestAccess::findTile(tileset, contentKey);
        if (contentTile &&
            contentTile->loadState == TileLoadState::Done &&
            contentTile->contentKind == TileContentKind::Render &&
            contentTile->gltfModel) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    check(contentTile &&
              contentTile->gltfModel &&
              contentTile->gltfModel->primitives.size() == 1u &&
              contentTile->gltfModel->primitives[0].instances.size() == 2u,
          "Tileset: default-up-axis i3dm loads two instances");
    if (!contentTile ||
        !contentTile->gltfModel ||
        contentTile->gltfModel->primitives.empty() ||
        contentTile->gltfModel->primitives[0].instances.size() < 2u) {
        std::filesystem::remove_all(root);
        return;
    }

    const GltfPrimitive& primitive =
        contentTile->gltfModel->primitives[0];
    const Vec3 yUpVertex = primitive.vertices[2].positionEcef;
    const Vec3 first =
        contentTile->gltfContentTransform *
        (primitive.instances[0].transform * yUpVertex);
    const Vec3 second =
        contentTile->gltfContentTransform *
        (primitive.instances[1].transform * yUpVertex);
    check(std::abs(first.x()) < 1e-12 &&
              std::abs(first.y()) < 1e-12 &&
              std::abs(first.z() - 1.0) < 1e-12 &&
              std::abs(second.x()) < 1e-12 &&
              std::abs(second.y() - 10.0) < 1e-12 &&
              std::abs(second.z() - 1.0) < 1e-12,
          "Tileset: default-up-axis i3dm keeps instance positions tile-local while converting glTF Y-up vertices");

    std::filesystem::remove_all(root);
}

void testTilesetJsonUnsupportedMultipleContentsFailsTile() {
    const std::string tilesetJson = R"json({
      "asset": {"version": "1.1"},
      "geometricError": 100,
      "root": {
        "boundingVolume": {"region": [-0.01, -0.01, 0.01, 0.01, 0, 100]},
        "geometricError": 64,
        "children": [{
          "boundingVolume": {"region": [-0.005, -0.005, 0.005, 0.005, 0, 50]},
          "geometricError": 16,
          "contents": [
            {"uri": "a.glb"},
            {"uri": "b.glb"}
          ]
        }]
      }
    })json";

    auto provider = std::make_unique<TilesetJsonContentProvider>(
        "file:///unsupported-multiple-contents/tileset.json",
        std::vector<uint8_t>(tilesetJson.begin(), tilesetJson.end()),
        "unsupported multiple contents fixture");
    TilesetJsonContentProvider* rawProvider = provider.get();
    check(rawProvider->valid(),
          "TilesetJsonContentProvider: unsupported contents fixture keeps parseable metadata");
    const std::vector<TileKey> roots = rawProvider->rootTiles();
    check(roots.size() == 1,
          "TilesetJsonContentProvider: unsupported contents fixture creates wrapper root");
    if (roots.empty()) return;

    const std::vector<TileKey> rootChildren =
        rawProvider->childTiles(roots.front());
    check(rootChildren.size() == 1,
          "TilesetJsonContentProvider: unsupported contents fixture keeps parsed root tile");
    if (rootChildren.empty()) return;

    const std::vector<TileKey> unsupportedChildren =
        rawProvider->childTiles(rootChildren.front());
    check(unsupportedChildren.size() == 1 &&
              rawProvider->supportsTile(unsupportedChildren.front()),
          "TilesetJsonContentProvider: unsupported contents child stays addressable");
    if (unsupportedChildren.empty()) return;

    DummyRenderDevice device;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{},
        std::move(provider));

    const TileKey unsupportedKey = unsupportedChildren.front();
    TilesetTestAccess::requestMissingTile(tileset, unsupportedKey);
    TilesetTestAccess::processPendingUploads(tileset);
    TilesetTile* unsupportedTile =
        TilesetTestAccess::findTile(tileset, unsupportedKey);
    check(unsupportedTile &&
              unsupportedTile->loadState == TileLoadState::Failed &&
              unsupportedTile->contentKind == TileContentKind::Unknown,
          "Tileset: unsupported multiple contents fails explicitly instead of disappearing");
}

void testTilesetJsonProviderParsesViewerRequestVolume() {
    const std::string tilesetJson = R"json({
      "asset": {"version": "1.1"},
      "geometricError": 100,
      "root": {
        "boundingVolume": {"region": [-0.01, -0.01, 0.01, 0.01, 0, 100]},
        "viewerRequestVolume": {"sphere": [10, 20, 30, 40]},
        "geometricError": 64,
        "content": {"uri": "tile.glb"}
      }
    })json";

    TilesetJsonContentProvider provider(
        "file:///viewer-request-volume/tileset.json",
        std::vector<uint8_t>(tilesetJson.begin(), tilesetJson.end()),
        "viewer request volume fixture");
    check(provider.valid(),
          "TilesetJsonContentProvider: viewerRequestVolume fixture parses");
    const std::vector<TileKey> roots = provider.rootTiles();
    if (roots.empty()) return;
    const std::vector<TileKey> rootChildren =
        provider.childTiles(roots.front());
    check(rootChildren.size() == 1,
          "TilesetJsonContentProvider: viewerRequestVolume root stays addressable");
    if (rootChildren.empty()) return;

    const std::optional<TilesetContentTileMetadata> metadata =
        provider.tileMetadata(rootChildren.front());
    check(metadata &&
              metadata->viewerRequestVolume &&
              metadata->viewerRequestVolume->kind ==
                  TileBoundingVolumeKind::Sphere &&
              metadata->viewerRequestVolume->sphere.getCenter() ==
                  Vec3(10.0, 20.0, 30.0) &&
              std::abs(
                  metadata->viewerRequestVolume->sphere.getRadius() -
                  40.0) < 1e-12,
          "TilesetJsonContentProvider: viewerRequestVolume sphere is preserved in metadata");
}

void expectTilesetJsonTileFailsExplicitly(
    std::unique_ptr<TilesetJsonContentProvider> provider,
    const TileKey& key,
    const std::string& name) {
    DummyRenderDevice device;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{},
        std::move(provider));

    TilesetTestAccess::requestMissingTile(tileset, key);
    TilesetTestAccess::processPendingUploads(tileset);
    TilesetTile* tile = TilesetTestAccess::findTile(tileset, key);
    check(tile &&
              tile->loadState == TileLoadState::Failed &&
              tile->contentKind == TileContentKind::Unknown,
          name);
}

void testTilesetJsonTopLevelUnknownRequiredExtensionInvalidatesProvider() {
    const std::string tilesetJson = R"json({
      "asset": {"version": "1.1"},
      "extensionsRequired": ["EARTH_unknown_required_extension"],
      "geometricError": 100,
      "root": {
        "boundingVolume": {"region": [-0.01, -0.01, 0.01, 0.01, 0, 100]},
        "geometricError": 64
      }
    })json";

    TilesetJsonContentProvider provider(
        "file:///unknown-required-extension/tileset.json",
        std::vector<uint8_t>(tilesetJson.begin(), tilesetJson.end()),
        "unknown required extension fixture");
    check(!provider.valid(),
          "TilesetJsonContentProvider: unknown top-level required extension invalidates provider");
    check(provider.rootTiles().empty(),
          "TilesetJsonContentProvider: invalid required-extension tileset exposes no roots");
}

void testTilesetJsonUnsupportedTileRequiredExtensionFailsTile() {
    const std::string tilesetJson = R"json({
      "asset": {"version": "1.1"},
      "geometricError": 100,
      "root": {
        "boundingVolume": {"region": [-0.01, -0.01, 0.01, 0.01, 0, 100]},
        "geometricError": 64,
        "content": {
          "uri": "tile.glb",
          "extensionsRequired": ["EARTH_content_decoder"]
        }
      }
    })json";

    auto provider = std::make_unique<TilesetJsonContentProvider>(
        "file:///unsupported-tile-required-extension/tileset.json",
        std::vector<uint8_t>(tilesetJson.begin(), tilesetJson.end()),
        "unsupported tile required extension fixture");
    TilesetJsonContentProvider* rawProvider = provider.get();
    check(rawProvider->valid(),
          "TilesetJsonContentProvider: unsupported tile extension keeps parseable metadata");
    const std::vector<TileKey> roots = rawProvider->rootTiles();
    if (roots.empty()) return;
    const std::vector<TileKey> rootChildren =
        rawProvider->childTiles(roots.front());
    check(rootChildren.size() == 1 &&
              rawProvider->supportsTile(rootChildren.front()),
          "TilesetJsonContentProvider: unsupported tile extension root stays addressable");
    if (rootChildren.empty()) return;

    expectTilesetJsonTileFailsExplicitly(
        std::move(provider),
        rootChildren.front(),
        "Tileset: unsupported tile required extension fails explicitly");
}

void testTilesetJsonUnsupportedImplicitTilingFailsTile() {
    auto runCase = [](const std::string& tilesetJson,
                      const std::string& url,
                      const std::string& name,
                      const std::string& checkName) {
        auto provider = std::make_unique<TilesetJsonContentProvider>(
            url,
            std::vector<uint8_t>(tilesetJson.begin(), tilesetJson.end()),
            name);
        TilesetJsonContentProvider* rawProvider = provider.get();
        check(rawProvider->valid(),
              checkName + ": implicit tiling fixture keeps parseable metadata");
        const std::vector<TileKey> roots = rawProvider->rootTiles();
        if (roots.empty()) return;
        const std::vector<TileKey> rootChildren =
            rawProvider->childTiles(roots.front());
        check(rootChildren.size() == 1 &&
                  rawProvider->supportsTile(rootChildren.front()),
              checkName + ": implicit tile stays addressable");
        if (rootChildren.empty()) return;

        expectTilesetJsonTileFailsExplicitly(
            std::move(provider),
            rootChildren.front(),
            checkName + ": unsupported implicit tiling fails explicitly");
    };

    const std::string coreImplicitJson = R"json({
      "asset": {"version": "1.1"},
      "geometricError": 100,
      "root": {
        "boundingVolume": {"region": [-0.01, -0.01, 0.01, 0.01, 0, 100]},
        "geometricError": 64,
        "content": {"uri": "tiles/{level}/{x}/{y}.glb"},
        "implicitTiling": {
          "subdivisionScheme": "QUADTREE",
          "subtreeLevels": 2,
          "availableLevels": 1,
          "subtrees": {"uri": "subtrees/{level}/{x}/{y}.subtree"}
        }
      }
    })json";
    runCase(coreImplicitJson,
            "file:///unsupported-implicit-core/tileset.json",
            "unsupported core implicit tiling fixture",
            "TilesetJsonContentProvider");

    const std::string legacyImplicitJson = R"json({
      "asset": {"version": "1.0"},
      "extensionsUsed": ["3DTILES_implicit_tiling"],
      "extensionsRequired": ["3DTILES_implicit_tiling"],
      "geometricError": 100,
      "root": {
        "boundingVolume": {"region": [-0.01, -0.01, 0.01, 0.01, 0, 100]},
        "geometricError": 64,
        "content": {"uri": "tiles/{level}/{x}/{y}.b3dm"},
        "extensions": {
          "3DTILES_implicit_tiling": {
            "subdivisionScheme": "QUADTREE",
            "subtreeLevels": 2,
            "maximumLevel": 1,
            "subtrees": {"uri": "subtrees/{level}/{x}/{y}.subtree"}
          }
        }
      }
    })json";
    runCase(legacyImplicitJson,
            "file:///unsupported-implicit-legacy/tileset.json",
            "unsupported legacy implicit tiling fixture",
            "TilesetJsonContentProvider legacy");
}

void testTilesetJsonProviderLoadsExternalTilesetContent() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth-md-tileset-json-external";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "external");
    writeBytes(root / "triangle.glb", makeTriangleGlbBytes());

    const std::string externalTilesetJson = R"json({
      "asset": {"version": "1.1"},
      "geometricError": 50,
      "root": {
        "boundingVolume": {"sphere": [1, 2, 3, 10]},
        "geometricError": 8,
        "transform": [1,0,0,0, 0,1,0,0, 0,0,1,0, 5,0,0,1],
        "content": {"uri": "../triangle.glb"}
      }
    })json";
    writeBytes(root / "external" / "tileset.json",
               std::vector<uint8_t>(
                   externalTilesetJson.begin(),
                   externalTilesetJson.end()));

    const std::string parentTilesetJson = R"json({
      "asset": {"version": "1.1"},
      "geometricError": 100,
      "root": {
        "boundingVolume": {"region": [-0.01, -0.01, 0.01, 0.01, 0, 100]},
        "geometricError": 32,
        "transform": [1,0,0,0, 0,1,0,0, 0,0,1,0, 100,0,0,1],
        "content": {"uri": "external/tileset.json"}
      }
    })json";
    auto provider = std::make_unique<TilesetJsonContentProvider>(
        "file://" + (root / "tileset.json").generic_string(),
        std::vector<uint8_t>(
            parentTilesetJson.begin(),
            parentTilesetJson.end()),
        "external tileset fixture");
    TilesetJsonContentProvider* rawProvider = provider.get();
    const std::vector<TileKey> roots = rawProvider->rootTiles();
    if (roots.empty()) return;
    const std::vector<TileKey> rootChildren =
        rawProvider->childTiles(roots.front());
    if (rootChildren.empty()) return;
    const TileKey externalContentKey = rootChildren.front();

    DummyRenderDevice device;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{},
        std::move(provider));

    TilesetTestAccess::requestMissingTile(tileset, externalContentKey);
    TilesetTile* externalContentTile = nullptr;
    for (int i = 0; i < 100; ++i) {
        TilesetTestAccess::processPendingUploads(tileset);
        externalContentTile =
            TilesetTestAccess::findTile(tileset, externalContentKey);
        if (externalContentTile &&
            externalContentTile->loadState == TileLoadState::Done &&
            externalContentTile->contentKind == TileContentKind::External &&
            !externalContentTile->children.empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    check(externalContentTile &&
              externalContentTile->unconditionallyRefine &&
              !externalContentTile->children.empty(),
          "Tileset: JSON content tileset loads as external content and attaches child root");
    if (!externalContentTile || externalContentTile->children.empty()) return;

    const TileKey renderKey = externalContentTile->children.front()->key;
    TilesetTestAccess::requestMissingTile(tileset, renderKey);
    TilesetTile* renderTile = nullptr;
    for (int i = 0; i < 100; ++i) {
        TilesetTestAccess::processPendingUploads(tileset);
        renderTile = TilesetTestAccess::findTile(tileset, renderKey);
        if (renderTile &&
            renderTile->loadState == TileLoadState::Done &&
            renderTile->contentKind == TileContentKind::Render &&
            renderTile->gltfModel) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    check(renderTile &&
              renderTile->boundingVolume &&
              renderTile->boundingVolume->kind == TileBoundingVolumeKind::Sphere,
          "Tileset: external tileset child keeps its transformed sphere bounding volume");
    check(renderTile &&
              renderTile->gltfModel &&
              std::abs((renderTile->gltfContentTransform *
                        renderTile->gltfModel->primitives[0].vertices[0]
                            .positionEcef).x() - 105.0) < 1e-12,
          "Tileset: external tileset parent and child transforms compose into glTF content");

    std::filesystem::remove_all(root);
}

void testTilesetViewerRequestVolumeGatesContentLoadQueue() {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    auto provider = std::make_unique<ManualCompletionContentProvider>(
        rootKey);
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        nullptr,
        TilesetOptions{},
        std::move(provider));

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: viewerRequestVolume gate root tile is created");
    if (!root) return;

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(center + center.normalized() * 1000000.0,
                  center,
                  Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 126;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());

    root->viewerRequestVolume = TileBoundingVolume::fromSphere(
        camera.position() - camera.direction() * 1000000.0,
        100.0);
    TilesetTestAccess::selectTiles(tileset, frameState);
    check(TilesetTestAccess::loadQueueEmpty(tileset),
          "Tileset: viewerRequestVolume outside camera prevents content load queue");
    check(tileset.tilePlan().visibleTiles.empty(),
          "Tileset: viewerRequestVolume outside camera prevents rendering selection");

    frameState.frameId = 127;
    root->viewerRequestVolume = TileBoundingVolume::fromSphere(
        camera.position(),
        100.0);
    TilesetTestAccess::selectTiles(tileset, frameState);
    check(!TilesetTestAccess::loadQueueEmpty(tileset),
          "Tileset: viewerRequestVolume containing camera allows content load queue");
}

void testTilesetUnloadRenderContentReleasesGltfResources() {
    auto terrainProvider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::move(terrainProvider),
        std::move(scheme),
        {},
        nullptr,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: glTF unload root tile is created");
    if (!root) return;

    root->gltfModel = makeTriangleGltfModel();
    GltfPrimitiveRenderResources resources;
    resources.vertexBuffer = std::make_unique<DummyBuffer>(96);
    resources.indexBuffer = std::make_unique<DummyBuffer>(12);
    resources.vertexCount = 3;
    resources.indexCount = 3;
    root->gltfPrimitiveResources.push_back(std::move(resources));
    root->meshReady = true;
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::Done;

    TilesetTestAccess::unloadTileContent(tileset, *root, nullptr);

    check(root->gltfModel == nullptr &&
              root->gltfPrimitiveResources.empty(),
          "Tileset: unloadTileContent releases glTF model and primitive resources");
    check(root->contentKind == TileContentKind::Unknown &&
              root->loadState == TileLoadState::Unloaded &&
              !root->meshReady,
          "Tileset: unloaded glTF render content returns to native unknown/unloaded state");
}

void testTilesetTotalBytesIncludesDecodedHeightmapPayload() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    auto heightmap = makeFlatHeightmap(7.0f);
    heightmap->rawData = {1, 2, 3};
    heightmap->noDataValues = {-32768.0f};
    const int64_t expected =
        static_cast<int64_t>(heightmap->rawData.size()) +
        static_cast<int64_t>(heightmap->heights.size() * sizeof(float)) +
        static_cast<int64_t>(heightmap->noDataValues.size() * sizeof(float));

    TilesetTestAccess::putTerrainCache(tileset, rootKey, std::move(heightmap));
    TilesetTestAccess::updateTotalBytesUsed(tileset);
    check(tileset.totalBytesUsed() == expected,
          "Tileset: totalBytesUsed includes decoded heightmap payload");
}

void testTerrainProviderDefaultAvailabilityHonorsZoomRange() {
    DefaultZoomTerrainProvider provider;

    check(provider.supportsTile(TileKey{"Geographic-TMS", 1, 0, 0}),
          "TerrainProvider: default availability accepts max zoom tile");
    check(!provider.supportsTile(TileKey{"Geographic-TMS", 2, 0, 0}),
          "TerrainProvider: default availability rejects tiles past max zoom");
}

void testTilesetCanRefineQuantizedMeshPastLayerMaxzoomWhenAvailable() {
    auto provider = std::make_unique<QuantizedMeshTerrainProvider>(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 1,
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}],
        [{"startX":0,"startY":0,"endX":0,"endY":0}],
        [{"startX":0,"startY":0,"endX":0,"endY":0}]
      ]
    })json";
    check(provider->configureFromLayerJson(
              layerJson, "https://example.invalid/layer.json"),
          "Tileset: maxzoom-refine layer configures");
    check(provider->supportsTile(TileKey{"Geographic-TMS", 2, 0, 0}),
          "Tileset: provider sees explicit availability past maxzoom");

    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});
    TilesetTile* parent =
        TilesetTestAccess::ensureTile(tileset, TileKey{"Geographic-TMS", 1, 0, 0});
    check(parent != nullptr,
          "Tileset: maxzoom-refine parent tile is created");
    check(parent && TilesetTestAccess::canRefine(tileset, *parent),
          "Tileset: refinement follows child availability instead of layer maxzoom");

    if (parent) {
        TilesetTestAccess::ensureTileChildren(tileset, *parent);
        check(!parent->children.empty() && !parent->children[0]->upsampledFromParent,
              "Tileset: explicit child past maxzoom is created as real terrain");
    }
}

void testTilesetRequestsExplicitAvailabilityPastProviderMaxzoom() {
    auto requestedKeys = std::make_shared<std::vector<TileKey>>();
    auto provider =
        std::make_unique<ExplicitPastMaxZoomTerrainProvider>(requestedKeys);
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey explicitKey{"Geographic-TMS", 2, 0, 0};
    TilesetTestAccess::requestMissingTile(tileset, explicitKey);

    check(requestedKeys->size() == 1 && requestedKeys->front() == explicitKey,
          "Tileset: request queue keeps explicit availability past provider maxzoom");
}

void testTilesetRetryLaterRemainsRetryable() {
    auto provider = std::make_unique<TerminalTerrainProvider>(
        TerrainTileLoadStatus::RetryLater);
    TerminalTerrainProvider* rawProvider = provider.get();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};

    TilesetTestAccess::ensureTile(tileset, rootKey);
    TilesetTestAccess::requestMissingTile(tileset, rootKey);
    TilesetTestAccess::processPendingUploads(tileset);
    TilesetTile* root = TilesetTestAccess::findTile(tileset, rootKey);
    check(root && root->loadState == TileLoadState::FailedTemporarily,
          "Tileset: RetryLater maps to FailedTemporarily");
    check(root && root->contentKind == TileContentKind::Unknown,
          "Tileset: RetryLater keeps content unknown");

    TilesetTestAccess::requestMissingTile(tileset, rootKey);
    check(rawProvider->requestCount == 2,
          "Tileset: FailedTemporarily terrain remains retryable");
}

void testTilesetFailedTerminalDoesNotRetry() {
    auto provider = std::make_unique<TerminalTerrainProvider>(
        TerrainTileLoadStatus::Failed);
    TerminalTerrainProvider* rawProvider = provider.get();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};

    TilesetTestAccess::ensureTile(tileset, rootKey);
    TilesetTestAccess::requestMissingTile(tileset, rootKey);
    TilesetTestAccess::processPendingUploads(tileset);
    TilesetTile* root = TilesetTestAccess::findTile(tileset, rootKey);
    check(root && root->loadState == TileLoadState::Failed,
          "Tileset: Failed maps to terminal Failed state");
    check(root && TilesetTestAccess::isTileRenderable(tileset, *root),
          "Tileset: terminal Failed tile is renderable like cesium-native empty content");

    TilesetTestAccess::requestMissingTile(tileset, rootKey);
    check(rawProvider->requestCount == 1,
          "Tileset: terminal Failed terrain is not retried by normal queue");
}

void testTilesetEmptyContentReachesDone() {
    auto provider = std::make_unique<TerminalTerrainProvider>(
        TerrainTileLoadStatus::Empty);
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};

    TilesetTestAccess::ensureTile(tileset, rootKey);
    TilesetTestAccess::requestMissingTile(tileset, rootKey);
    TilesetTestAccess::processPendingUploads(tileset);
    TilesetTile* root = TilesetTestAccess::findTile(tileset, rootKey);
    check(root && root->loadState == TileLoadState::Done,
          "Tileset: Empty content completes to Done like cesium-native");
    check(root && root->contentKind == TileContentKind::Empty,
          "Tileset: Empty content keeps explicit Empty kind");
    check(root && TilesetTestAccess::isTileRenderable(tileset, *root),
          "Tileset: Empty Done content is renderable like cesium-native");
}

void testTilesetRenderContentRequiresDoneState() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: renderability state root tile is created");
    if (!root) return;

    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(0.0f));
    root->loadState = TileLoadState::ContentLoaded;
    root->contentKind = TileContentKind::Render;

    check(!TilesetTestAccess::isTileRenderable(tileset, *root),
          "Tileset: ContentLoaded render content is not renderable before main-thread finish");

    TilesetTestAccess::ensureTileMesh(tileset, *root);
    check(root->loadState == TileLoadState::Done &&
              TilesetTestAccess::isTileRenderable(tileset, *root),
          "Tileset: Done render content is renderable after main-thread finish");
}

void testTilesetMappedRasterMustBeReadyForRenderability() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: mapped-raster renderability root tile is created");
    if (!root) return;

    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(0.0f));
    TilesetTestAccess::ensureTileMesh(tileset, *root);
    check(TilesetTestAccess::isTileRenderable(tileset, *root),
          "Tileset: Done render content without mapped rasters is renderable");

    root->rasterOverlays.resize(1);
    root->rasterOverlays[0] = std::make_unique<RasterMappedToTilesetTile>();
    check(!TilesetTestAccess::isTileRenderable(tileset, *root),
          "Tileset: mapped raster without ready tile blocks renderability like cesium-native");
}

void testTilesetUnconditionallyRefineRenderableOnlyWithoutChildren() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};

    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: unconditionally-refine renderability root tile is created");
    if (!root) return;

    root->loadState = TileLoadState::Done;
    root->contentKind = TileContentKind::Empty;
    root->unconditionallyRefine = true;
    check(TilesetTestAccess::isTileRenderable(tileset, *root),
          "Tileset: unconditionally-refined leaf is renderable when waiting is futile");

    TilesetTile* child = TilesetTestAccess::ensureTile(
        tileset,
        TileKey{"Geographic-TMS", 1, 0, 0});
    check(child != nullptr && !root->children.empty(),
          "Tileset: unconditionally-refine renderability child is linked");
    check(!TilesetTestAccess::isTileRenderable(tileset, *root),
          "Tileset: unconditionally-refined tile with children is not renderable");
}

void testTilesetMainThreadLoadingTimeLimitZeroDrainsPendingUploads() {
    auto provider = std::make_unique<TerminalTerrainProvider>(
        TerrainTileLoadStatus::Success);
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const std::vector<TileKey> keys = {
        {"Geographic-TMS", 1, 0, 0},
        {"Geographic-TMS", 1, 1, 0},
        {"Geographic-TMS", 1, 2, 0}
    };

    for (const TileKey& key : keys) {
        TilesetTestAccess::ensureTile(tileset, key);
        TilesetTestAccess::requestMissingTile(tileset, key);
    }

    check(tileset.loadDiagnostics().pendingTerrainUploads ==
              static_cast<int>(keys.size()),
          "Tileset: success requests queue pending terrain uploads");

    TilesetTestAccess::processPendingUploads(tileset);

    const TilesetLoadDiagnostics diag = tileset.loadDiagnostics();
    bool allDone = diag.pendingTerrainUploads == 0;
    for (const TileKey& key : keys) {
        TilesetTile* tile = TilesetTestAccess::findTile(tileset, key);
        allDone =
            allDone &&
            tile &&
            tile->loadState == TileLoadState::Done &&
            tile->contentKind == TileContentKind::Render &&
            TilesetTestAccess::isTileRenderable(tileset, *tile);
    }

    check(allDone,
          "Tileset: mainThreadLoadingTimeLimit=0 drains all pending uploads like cesium-native");
}

void testTilesetMainThreadPendingUploadsUseNativePriority() {
    TilesetOptions options;
    options.maximumSimultaneousTileLoads = 2;
    options.mainThreadLoadingTimeLimit = 1.0e-12;

    auto provider = std::make_unique<ManualCompletionTerrainProvider>();
    ManualCompletionTerrainProvider* rawProvider = provider.get();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, options);

    const TileKey preloadKey{"Geographic-TMS", 1, 0, 0};
    const TileKey urgentKey{"Geographic-TMS", 1, 1, 0};
    TilesetTestAccess::ensureTile(tileset, preloadKey);
    TilesetTestAccess::ensureTile(tileset, urgentKey);

    TilesetTestAccess::requestMissingPreload(tileset, preloadKey);
    TilesetTestAccess::requestMissingUrgent(tileset, urgentKey);
    check(rawProvider->pendingRequests.size() == 2,
          "Tileset: priority test queues both terrain requests");

    check(rawProvider->completeWithHeightmap(
              preloadKey,
              makeFlatHeightmap(1.0f)),
          "Tileset: low-priority terrain can complete first");
    check(rawProvider->completeWithHeightmap(
              urgentKey,
              makeFlatHeightmap(2.0f)),
          "Tileset: urgent terrain can complete second");
    check(tileset.loadDiagnostics().pendingTerrainUploads == 2,
          "Tileset: both completed requests wait for main-thread upload");

    TilesetTestAccess::processPendingUploads(tileset);

    TilesetTile* preloadTile =
        TilesetTestAccess::findTile(tileset, preloadKey);
    TilesetTile* urgentTile =
        TilesetTestAccess::findTile(tileset, urgentKey);
    check(urgentTile &&
              urgentTile->loadState == TileLoadState::Done &&
              urgentTile->contentKind == TileContentKind::Render &&
              TilesetTestAccess::isTileRenderable(tileset, *urgentTile),
          "Tileset: budgeted main-thread upload processes urgent terrain first");
    check(preloadTile &&
              preloadTile->loadState == TileLoadState::ContentLoading &&
              tileset.loadDiagnostics().pendingTerrainUploads == 1,
          "Tileset: lower-priority preload remains pending when budget expires");
}

void testTilesetLoadQueueKeepsTraversalPriority() {
    const TileKey lowPriorityKey{"Geographic-TMS", 1, 0, 0};
    const TileKey highPriorityKey{"Geographic-TMS", 1, 1, 0};

    {
        TilesetOptions options;
        options.maximumSimultaneousTileLoads = 1;

        auto provider = std::make_unique<ManualCompletionTerrainProvider>();
        ManualCompletionTerrainProvider* rawProvider = provider.get();
        auto scheme = TileScheme::createGeographicTMS();
        Tileset tileset(
            std::move(provider),
            std::move(scheme),
            {},
            nullptr,
            options);

        TilesetTestAccess::ensureTile(tileset, lowPriorityKey);
        TilesetTestAccess::ensureTile(tileset, highPriorityKey);

        TilesetTestAccess::requestMissingTilesWithPriorities(
            tileset,
            lowPriorityKey,
            100.0,
            highPriorityKey,
            1.0);

        check(rawProvider->pendingRequests.size() == 1 &&
                  rawProvider->pendingRequests.front().key == highPriorityKey,
              "Tileset: worker request uses selector-computed tile priority");
        check(rawProvider->completeWithHeightmap(
                  highPriorityKey,
                  makeFlatHeightmap(1.0f)),
              "Tileset: priority worker request can complete");
    }

    {
        TilesetOptions options;
        options.maximumSimultaneousTileLoads = 2;
        options.mainThreadLoadingTimeLimit = 1.0e-12;

        auto provider = std::make_unique<ManualCompletionTerrainProvider>();
        ManualCompletionTerrainProvider* rawProvider = provider.get();
        auto scheme = TileScheme::createGeographicTMS();
        Tileset tileset(
            std::move(provider),
            std::move(scheme),
            {},
            nullptr,
            options);

        TilesetTestAccess::ensureTile(tileset, lowPriorityKey);
        TilesetTestAccess::ensureTile(tileset, highPriorityKey);

        TilesetTestAccess::requestMissingTilesWithPriorities(
            tileset,
            lowPriorityKey,
            100.0,
            highPriorityKey,
            1.0);

        check(rawProvider->pendingRequests.size() == 2,
              "Tileset: priority upload test queues both terrain requests");
        check(rawProvider->completeWithHeightmap(
                  lowPriorityKey,
                  makeFlatHeightmap(1.0f)),
              "Tileset: lower selector-priority terrain can complete first");
        check(rawProvider->completeWithHeightmap(
                  highPriorityKey,
                  makeFlatHeightmap(2.0f)),
              "Tileset: higher selector-priority terrain can complete second");

        TilesetTestAccess::processPendingUploads(tileset);

        TilesetTile* lowPriorityTile =
            TilesetTestAccess::findTile(tileset, lowPriorityKey);
        TilesetTile* highPriorityTile =
            TilesetTestAccess::findTile(tileset, highPriorityKey);
        check(highPriorityTile &&
                  highPriorityTile->loadState == TileLoadState::Done &&
                  highPriorityTile->contentKind == TileContentKind::Render,
              "Tileset: main-thread upload uses preserved selector priority");
        check(lowPriorityTile &&
                  lowPriorityTile->loadState == TileLoadState::ContentLoading &&
                  tileset.loadDiagnostics().pendingTerrainUploads == 1,
              "Tileset: lower selector-priority upload remains pending");
    }
}

void testTilesetMainThreadUploadBudgetIsGlobalAcrossContentKinds() {
    TilesetOptions options;
    options.maximumSimultaneousTileLoads = 2;
    options.mainThreadLoadingTimeLimit = 1.0e-12;

    const TileKey terrainKey{"Geographic-TMS", 1, 0, 0};
    const TileKey contentKey{"Geographic-TMS", 1, 1, 0};

    auto terrainProvider =
        std::make_unique<ManualCompletionTerrainProvider>();
    ManualCompletionTerrainProvider* rawTerrainProvider =
        terrainProvider.get();
    auto contentProvider =
        std::make_unique<ManualCompletionContentProvider>(contentKey);
    ManualCompletionContentProvider* rawContentProvider =
        contentProvider.get();
    auto scheme = TileScheme::createGeographicTMS();
    DummyRenderDevice device;
    Tileset tileset(
        std::move(terrainProvider),
        std::move(scheme),
        {},
        &device,
        options,
        std::move(contentProvider));

    TilesetTestAccess::ensureTile(tileset, terrainKey);
    TilesetTestAccess::ensureTile(tileset, contentKey);
    TilesetTestAccess::requestMissingTilesWithPriorities(
        tileset,
        terrainKey,
        100.0,
        contentKey,
        1.0);

    check(rawTerrainProvider->pendingRequests.size() == 1 &&
              rawContentProvider->pendingRequests.size() == 1,
          "Tileset: mixed upload budget test queues terrain and content requests");
    check(rawTerrainProvider->completeWithHeightmap(
              terrainKey,
              makeFlatHeightmap(1.0f)),
          "Tileset: lower-priority terrain upload can complete first");
    check(rawContentProvider->completeWithModel(
              contentKey,
              makeTriangleGltfModel()),
          "Tileset: higher-priority content upload can complete second");
    check(tileset.loadDiagnostics().pendingTerrainUploads == 1 &&
              tileset.loadDiagnostics().pendingContentUploads == 1,
          "Tileset: mixed terrain/content uploads wait on main thread");

    TilesetTestAccess::processPendingUploads(tileset);

    TilesetTile* terrainTile =
        TilesetTestAccess::findTile(tileset, terrainKey);
    TilesetTile* contentTile =
        TilesetTestAccess::findTile(tileset, contentKey);
    const TilesetLoadDiagnostics diag = tileset.loadDiagnostics();
    check(contentTile &&
              contentTile->loadState == TileLoadState::Done &&
              contentTile->contentKind == TileContentKind::Render &&
              contentTile->gltfModel &&
              !contentTile->gltfPrimitiveResources.empty(),
          "Tileset: global main-thread budget processes higher-priority content first");
    check(terrainTile &&
              terrainTile->loadState == TileLoadState::ContentLoading &&
              diag.pendingTerrainUploads == 1 &&
              diag.pendingContentUploads == 0,
          "Tileset: lower-priority terrain waits under the same upload budget");
}

void testTilesetPendingMainThreadUploadsDoNotConsumeWorkerSlots() {
    TilesetOptions options;
    options.maximumSimultaneousTileLoads = 2;
    options.mainThreadLoadingTimeLimit = 1.0e-12;

    auto provider = std::make_unique<ManualCompletionTerrainProvider>();
    ManualCompletionTerrainProvider* rawProvider = provider.get();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, options);

    const TileKey firstKey{"Geographic-TMS", 1, 0, 0};
    const TileKey secondKey{"Geographic-TMS", 1, 1, 0};
    const TileKey thirdKey{"Geographic-TMS", 1, 2, 0};

    TilesetTestAccess::requestMissingTile(tileset, firstKey);
    TilesetTestAccess::requestMissingTile(tileset, secondKey);
    check(rawProvider->pendingRequests.size() == 2,
          "Tileset: worker-slot test starts two worker requests");

    check(rawProvider->completeWithHeightmap(
              firstKey,
              makeFlatHeightmap(1.0f)),
          "Tileset: worker-slot first request can complete to main-thread queue");
    check(rawProvider->completeWithHeightmap(
              secondKey,
              makeFlatHeightmap(2.0f)),
          "Tileset: worker-slot second request can complete to main-thread queue");
    check(tileset.loadDiagnostics().pendingTerrainUploads == 2,
          "Tileset: worker-slot completed loads wait for main-thread upload");

    TilesetTestAccess::requestMissingTile(tileset, thirdKey);
    check(rawProvider->pendingRequests.size() == 1 &&
              rawProvider->pendingRequests.front().key == thirdKey,
          "Tileset: pending main-thread uploads do not consume worker load slots");

    check(rawProvider->completeWithHeightmap(
              thirdKey,
              makeFlatHeightmap(3.0f)),
          "Tileset: worker-slot third request is completed for cleanup");
}

void testTilesetDestructorWaitsForPendingTerrainCallbacks() {
    const TileKey key{"Geographic-TMS", 1, 0, 0};
    std::atomic<bool> completionCalled{false};
    std::thread completer;

    {
        auto provider = std::make_unique<ManualCompletionTerrainProvider>();
        ManualCompletionTerrainProvider* rawProvider = provider.get();
        auto scheme = TileScheme::createGeographicTMS();
        Tileset tileset(
            std::move(provider),
            std::move(scheme),
            {},
            nullptr,
            TilesetOptions{});

        TilesetTestAccess::ensureTile(tileset, key);
        TilesetTestAccess::requestMissingTile(tileset, key);
        check(rawProvider->pendingRequests.size() == 1,
              "Tileset: destructor test has one pending terrain callback");

        completer = std::thread([rawProvider, key, &completionCalled]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            completionCalled.store(
                rawProvider->completeWithHeightmap(
                    key,
                    makeFlatHeightmap(3.0f)),
                std::memory_order_release);
        });
    }

    completer.join();
    check(completionCalled.load(std::memory_order_acquire),
          "Tileset: destructor waits until pending terrain callback exits");
}

void testTilesetForbidHolesCarriesCulledTileRenderability() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();

    TilesetOptions options;
    options.forbidHoles = true;
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, options);

    const TileKey key{"Geographic-TMS", 1, 0, 0};
    TilesetTile* tile = TilesetTestAccess::ensureTile(tileset, key);
    check(tile != nullptr,
          "Tileset: forbidHoles culled-detail tile is created");
    if (!tile) return;

    tile->refine = TileRefine::Replace;
    check(TilesetTestAccess::culledTileReportsMissingRenderableForForbidHoles(
              tileset,
              *tile),
          "Tileset: forbidHoles culled replacement tile reports missing content");
}

void testTilesetCulledTileDetailsIgnorePreloadOnlyTiles() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();

    TilesetOptions options;
    options.forbidHoles = false;
    options.preloadSiblings = true;
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, options);

    const TileKey key{"Geographic-TMS", 1, 0, 0};
    TilesetTile* tile = TilesetTestAccess::ensureTile(tileset, key);
    check(tile != nullptr,
          "Tileset: preload-only culled-detail tile is created");
    if (!tile) return;

    tile->refine = TileRefine::Replace;
    check(TilesetTestAccess::culledTileIsIgnoredWithoutForbidHoles(
              tileset,
              *tile),
          "Tileset: preload-only culled tile does not constrain traversal details");
}

void testTilesetLoadDiagnosticsExposeNativeLifecycleStates() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const std::vector<TileKey> keys = {
        {"Geographic-TMS", 3, 0, 0},
        {"Geographic-TMS", 3, 1, 0},
        {"Geographic-TMS", 3, 2, 0},
        {"Geographic-TMS", 3, 3, 0},
        {"Geographic-TMS", 3, 4, 0},
        {"Geographic-TMS", 3, 5, 0},
        {"Geographic-TMS", 3, 6, 0}
    };

    std::vector<TilesetTile*> tiles;
    tiles.reserve(keys.size());
    for (const TileKey& key : keys) {
        tiles.push_back(TilesetTestAccess::ensureTile(tileset, key));
    }
    if (tiles.size() != keys.size() ||
        std::any_of(tiles.begin(), tiles.end(),
                    [](const TilesetTile* tile) { return tile == nullptr; })) {
        check(false, "Tileset: load diagnostics setup creates tiles");
        return;
    }

    const TilesetLoadDiagnostics baseline = tileset.loadDiagnostics();

    tiles[0]->loadState = TileLoadState::Unloading;
    tiles[0]->contentKind = TileContentKind::Unknown;
    tiles[1]->loadState = TileLoadState::FailedTemporarily;
    tiles[1]->contentKind = TileContentKind::Empty;
    tiles[2]->loadState = TileLoadState::Unloaded;
    tiles[2]->contentKind = TileContentKind::External;
    tiles[3]->loadState = TileLoadState::ContentLoading;
    tiles[3]->contentKind = TileContentKind::Render;
    tiles[4]->loadState = TileLoadState::ContentLoaded;
    tiles[4]->contentKind = TileContentKind::Unknown;
    tiles[5]->loadState = TileLoadState::Done;
    tiles[5]->contentKind = TileContentKind::Render;
    tiles[6]->loadState = TileLoadState::Failed;
    tiles[6]->contentKind = TileContentKind::Unknown;
    tiles[6]->missingRasterOverlayProjections.push_back(
        RasterOverlayProjection::Geographic);

    TilesetTestAccess::queuePreload(tileset, keys[0]);
    TilesetTestAccess::queueNormal(tileset, keys[1]);
    TilesetTestAccess::queueUrgent(tileset, keys[2]);
    TilesetTestAccess::markEligibleForUnloading(tileset, keys[5]);

    const TilesetLoadDiagnostics diag = tileset.loadDiagnostics();
    check(diag.loadQueuePreloadRequests == 1 &&
              diag.loadQueueNormalRequests == 1 &&
              diag.loadQueueUrgentRequests == 1 &&
              diag.loadQueueTotal() == 3,
          "Tileset: load diagnostics exposes priority request groups");
    check(diag.loadUnloadingTiles == baseline.loadUnloadingTiles + 1 &&
              diag.loadFailedTemporarilyTiles ==
                  baseline.loadFailedTemporarilyTiles + 1 &&
              diag.loadUnloadedTiles == baseline.loadUnloadedTiles - 6 &&
              diag.loadContentLoadingTiles ==
                  baseline.loadContentLoadingTiles + 1 &&
              diag.loadContentLoadedTiles ==
                  baseline.loadContentLoadedTiles + 1 &&
              diag.loadDoneTiles == baseline.loadDoneTiles + 1 &&
              diag.loadFailedTiles == baseline.loadFailedTiles + 1,
          "Tileset: load diagnostics counts every TileLoadState");
    check(diag.contentUnknownTiles == baseline.contentUnknownTiles - 4 &&
              diag.contentEmptyTiles == baseline.contentEmptyTiles + 1 &&
              diag.contentExternalTiles == baseline.contentExternalTiles + 1 &&
              diag.contentRenderTiles == baseline.contentRenderTiles + 2,
          "Tileset: load diagnostics counts content kinds separately");
    check(diag.unloadQueueTiles == 1,
          "Tileset: load diagnostics exposes unload queue size");
    check(diag.missingRasterOverlayProjections ==
              baseline.missingRasterOverlayProjections + 1,
          "Tileset: load diagnostics exposes missing raster overlay projections");
}

void prepareSparseRootChildrenRenderable(Tileset& tileset,
                                         TilesetTile& root,
                                         const TileKey& rootKey) {
    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(0.0f));
    TilesetTestAccess::ensureTileMesh(tileset, root);
    TilesetTestAccess::ensureTileChildren(tileset, root);

    for (TilesetTile* child : root.children) {
        if (!child) continue;
        if (!child->upsampledFromParent) {
            TilesetTestAccess::putTerrainCache(
                tileset,
                child->key,
                makeFlatHeightmap(0.0f));
        }
        TilesetTestAccess::ensureTileMesh(tileset, *child);
    }
}

void testTilesetAdditiveRefinementRendersParentAndChildren() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: additive-refine root tile is created");
    if (!root) return;

    prepareSparseRootChildrenRenderable(tileset, *root, rootKey);
    root->refine = TileRefine::Add;

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(center + center.normalized() * 1000000.0,
                  center,
                  Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 130;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    bool rootVisible = false;
    bool childVisible = false;
    for (const TileKey& key : tileset.tilePlan().visibleTiles) {
        rootVisible = rootVisible || key == rootKey;
        childVisible = childVisible ||
                       (key.schemeId == rootKey.schemeId &&
                        key.z == rootKey.z + 1 &&
                        key.x / 2 == rootKey.x &&
                        key.y / 2 == rootKey.y);
    }

    check(rootVisible && childVisible,
          "Tileset: ADD refinement renders parent and descendants together");
    check(tileset.tilePlan().renderingNodeCount ==
              static_cast<int>(tileset.tilePlan().visibleTiles.size()),
          "Tileset: render diagnostics count ADD parent render list entries");
}

void testTilesetAdditiveRefinementCullsWithParentBounds() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    TilesetTile* child = TilesetTestAccess::ensureTile(tileset, childKey);
    check(root != nullptr && child != nullptr,
          "Tileset: ADD culling parent and child are created");
    if (!root || !child) return;

    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(0.0f));
    TilesetTestAccess::ensureTileMesh(tileset, *root);
    root->refine = TileRefine::Add;
    root->children.clear();
    root->children.push_back(child);
    child->parent = root;

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(center + center.normalized() * 1000000.0,
                  center,
                  Vec3::unitZ());
    child->bounds = Rectangle(0.25, 0.25, 0.3, 0.3);
    child->boundingVolume = TileBoundingVolume::fromSphere(
        camera.position() - camera.direction() * 2000000.0,
        1000.0);

    FrameState frameState;
    frameState.frameId = 132;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    const bool rootVisible = std::find(
        tileset.tilePlan().visibleTiles.begin(),
        tileset.tilePlan().visibleTiles.end(),
        rootKey) != tileset.tilePlan().visibleTiles.end();
    check(rootVisible && root->selectionState == TileSelectionState::Rendered,
          "Tileset: ADD refinement culls with parent bounds like cesium-native");
}

void testTilesetAdditiveRefinedTileCountsAsPreviouslyRendered() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: ADD previous-refined root tile is created");
    if (!root) return;

    root->loadState = TileLoadState::Done;
    root->contentKind = TileContentKind::Empty;
    root->refine = TileRefine::Add;
    root->previousSelectionState = TileSelectionState::Refined;
    root->children.clear();

    check(TilesetTestAccess::singleTileDetailsAnyWereRenderedLastFrame(
              tileset,
              *root),
          "Tileset: ADD refined tile counts as previously rendered like cesium-native");
}

void testTilesetUnconditionallyRefineIgnoresSatisfiedSse() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: unconditionally-refine root tile is created");
    if (!root) return;

    prepareSparseRootChildrenRenderable(tileset, *root, rootKey);
    root->unconditionallyRefine = true;

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(center + center.normalized() * 80000000.0,
                  center,
                  Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 131;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    bool rootVisible = false;
    bool childVisible = false;
    bool rootRecordedAsRefined = false;
    for (const TileKey& key : tileset.tilePlan().visibleTiles) {
        rootVisible = rootVisible || key == rootKey;
        childVisible = childVisible ||
                       (key.schemeId == rootKey.schemeId &&
                        key.z == rootKey.z + 1 &&
                        key.x / 2 == rootKey.x &&
                        key.y / 2 == rootKey.y);
    }
    for (const TileSelectionRecord& record : tileset.tilePlan().selectionRecords) {
        if (record.key == rootKey) {
            rootRecordedAsRefined =
                record.state == TileSelectionState::Refined;
            break;
        }
    }

    check(!rootVisible && childVisible && rootRecordedAsRefined,
          "Tileset: unconditionallyRefine visits children even when parent meets SSE");
}

void testTilesetUnconditionalChildDisablesChildrenBoundsCulling() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    TilesetTile* child = TilesetTestAccess::ensureTile(tileset, childKey);
    check(root != nullptr && child != nullptr,
          "Tileset: unconditional-child culling parent and child are created");
    if (!root || !child) return;

    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(0.0f));
    TilesetTestAccess::ensureTileMesh(tileset, *root);
    root->children.clear();
    root->children.push_back(child);
    child->parent = root;
    child->unconditionallyRefine = true;

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(center + center.normalized() * 80000000.0,
                  center,
                  Vec3::unitZ());
    child->bounds = Rectangle(0.25, 0.25, 0.3, 0.3);
    child->boundingVolume = TileBoundingVolume::fromSphere(
        camera.position() - camera.direction() * 2000000.0,
        1000.0);

    FrameState frameState;
    frameState.frameId = 133;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    const bool rootVisible = std::find(
        tileset.tilePlan().visibleTiles.begin(),
        tileset.tilePlan().visibleTiles.end(),
        rootKey) != tileset.tilePlan().visibleTiles.end();
    check(rootVisible && root->selectionState == TileSelectionState::Rendered,
          "Tileset: unconditional child disables children-bounds culling like cesium-native");
}

void testTilesetFogDensityTableIsConfigurable() {
    auto runWithFogTable =
        [](std::vector<FogDensityAtHeight> fogDensityTable) {
        auto provider = std::make_unique<SparseTerrainProvider>();
        auto scheme = TileScheme::createGeographicTMS();
        TilesetOptions options;
        options.fogDensityTable = std::move(fogDensityTable);
        Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, options);

        const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
        TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
        check(root != nullptr,
              "Tileset: fog-density configurable root tile is created");
        if (!root) return TilePlan{};

        const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
        Camera camera;
        camera.lookAt(center + center.normalized() * 100000.0,
                      center,
                      Vec3::unitZ());

        FrameState frameState;
        frameState.frameId = 132;
        frameState.camera = &camera;
        frameState.viewportWidthPixels = 800;
        frameState.viewportHeightPixels = 800;
        frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
        TilesetTestAccess::setLastCamera(
            tileset,
            camera.position(),
            camera.direction());
        TilesetTestAccess::selectTiles(tileset, frameState);
        return tileset.tilePlan();
    };

    TilePlan clearPlan = runWithFogTable({{0.0, 0.0}});
    TilePlan densePlan = runWithFogTable({{0.0, 1.0}, {1000000.0, 1.0}});

    check(!clearPlan.visibleTiles.empty(),
          "Tileset: zero fog density table permits root selection");
    check(densePlan.visibleTiles.empty() &&
              densePlan.notRenderingNodeCount > 0,
          "Tileset: custom dense fog density table culls selected roots");
}

void testTilesetRecordsAncestorMeetsSseForDescendants() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: ancestor-meets-SSE root tile is created");
    if (!root) return;

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    const Vec3 cameraPosition = center + center.normalized() * 80000000.0;
    Camera camera;
    camera.lookAt(cameraPosition, center, Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 77;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));

    root->selectionState = TileSelectionState::Refined;
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    bool descendantRecorded = false;
    for (const TileSelectionRecord& record : tileset.tilePlan().selectionRecords) {
        if (record.key.z > rootKey.z && record.ancestorMeetsSse) {
            descendantRecorded = true;
            break;
        }
    }
    check(descendantRecorded &&
              tileset.tilePlan().selectionAncestorMeetsSseCount > 0,
          "Tileset: descendants record ancestor-meets-SSE continuation like cesium-native");
}

void testTilesetUsesLargestSseAcrossSelectorViews() {
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    auto schemeForCenter = TileScheme::createGeographicTMS();
    const Rectangle rootBounds = schemeForCenter->tileToRectangle(rootKey);
    const Vec3 center = TilesetTestAccess::tileBoundsCenter(rootBounds);

    Camera nearCamera;
    nearCamera.lookAt(center + center.normalized() * 1000000.0,
                      center,
                      Vec3::unitZ());
    Camera farCamera;
    farCamera.lookAt(center + center.normalized() * 80000000.0,
                     center,
                     Vec3::unitZ());

    auto runSelection = [&](std::vector<FrameState::SelectorView> views) {
        auto provider = std::make_unique<SparseTerrainProvider>();
        auto scheme = TileScheme::createGeographicTMS();
        Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});
        TilesetTestAccess::ensureTile(tileset, rootKey);

        FrameState frameState;
        frameState.frameId = 91;
        frameState.camera = &nearCamera;
        frameState.viewportWidthPixels = 800;
        frameState.viewportHeightPixels = 800;
        frameState.selectorViews = std::move(views);
        TilesetTestAccess::setLastCamera(
            tileset,
            nearCamera.position(),
            nearCamera.direction());
        TilesetTestAccess::selectTiles(tileset, frameState);
        return tileset.tilePlan().maxVisibleZoom;
    };

    const int farOnlyZoom = runSelection({
        makeSelectorView(farCamera, 800, 800)});
    const int multiViewZoom = runSelection({
        makeSelectorView(farCamera, 800, 800),
        makeSelectorView(nearCamera, 800, 800)});

    check(farOnlyZoom == 0,
          "Tileset: far selector view alone keeps root when it meets SSE");
    check(multiViewZoom > farOnlyZoom,
          "Tileset: multiple selector views refine using largest SSE like cesium-native");
}

void testTilesetFogUsesEachSelectorViewDensity() {
    TilesetOptions options;
    options.fogDensityTable = {
        {0.0, 1.0},
        {1000000.0, 1.0},
        {10000000.0, 0.0}
    };

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    auto schemeForCenter = TileScheme::createGeographicTMS();
    const Rectangle rootBounds = schemeForCenter->tileToRectangle(rootKey);
    const Vec3 center = TilesetTestAccess::tileBoundsCenter(rootBounds);

    Camera lowFogCamera;
    lowFogCamera.lookAt(center + center.normalized() * 100000.0,
                        center,
                        Vec3::unitZ());
    Camera highClearCamera;
    highClearCamera.lookAt(center + center.normalized() * 80000000.0,
                           center,
                           Vec3::unitZ());

    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, options);
    TilesetTestAccess::ensureTile(tileset, rootKey);

    FrameState frameState;
    frameState.frameId = 92;
    frameState.camera = &lowFogCamera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews = {
        makeSelectorView(lowFogCamera, 800, 800),
        makeSelectorView(highClearCamera, 800, 800)
    };
    TilesetTestAccess::setLastCamera(
        tileset,
        lowFogCamera.position(),
        lowFogCamera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    check(!tileset.tilePlan().visibleTiles.empty(),
          "Tileset: fog culling uses each selector view density like cesium-native");
}

void testSceneSelectorViewOverrideFeedsMultipleViews() {
    Scene scene;
    scene.setViewport(800, 600, 1.0f);

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 target(ellipsoid.semiMajorAxis(), 0.0, 0.0);
    scene.camera().lookAt(
        target + Vec3(1000000.0, 0.0, 0.0),
        target,
        Vec3::unitZ());
    scene.update(1.0 / 60.0);
    check(scene.frameState().selectorViews.size() == 1,
          "Scene: default selector view comes from the main camera");

    Camera secondCamera;
    secondCamera.lookAt(
        target + Vec3(0.0, 1000000.0, 0.0),
        target,
        Vec3::unitZ());
    scene.setSelectorViewOverride({
        makeSelectorView(scene.camera(), 800, 600),
        makeSelectorView(secondCamera, 800, 600)});
    scene.update(1.0 / 60.0);
    check(scene.frameState().selectorViews.size() == 2,
          "Scene: selector view override exposes real multi-view selection input");

    scene.setSelectorViewOverride({});
    scene.update(1.0 / 60.0);
    check(scene.frameState().selectorViews.empty(),
          "Scene: empty selector override preserves native no-frustum semantics");

    scene.clearSelectorViewOverride();
    scene.update(1.0 / 60.0);
    check(scene.frameState().selectorViews.size() == 1,
          "Scene: clearing selector override returns to main-camera selection");
}

void testSceneOcclusionCallbackFeedsPrimaryAndAdditionalTilesets() {
    Scene scene;
    scene.setOcclusionCallback(
        [](const TilesetTile&) { return TileOcclusionState::Occluded; });

    auto makeTileset = []() {
        return std::make_unique<Tileset>(
            std::make_unique<SparseTerrainProvider>(),
            TileScheme::createGeographicTMS(),
            std::vector<ActivatedRasterOverlay*>{},
            nullptr,
            TilesetOptions{});
    };

    auto primaryTileset = makeTileset();
    Tileset* primaryRaw = primaryTileset.get();
    scene.setTileset(std::move(primaryTileset));

    auto additionalTileset = makeTileset();
    Tileset* additionalRaw = additionalTileset.get();
    scene.addTileset(std::move(additionalTileset));

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* primaryRoot =
        TilesetTestAccess::ensureTile(*primaryRaw, rootKey);
    TilesetTile* additionalRoot =
        TilesetTestAccess::ensureTile(*additionalRaw, rootKey);
    check(primaryRoot && additionalRoot,
          "Scene: occlusion callback test creates both tileset roots");
    if (!primaryRoot || !additionalRoot) return;

    check(TilesetTestAccess::checkOcclusion(*primaryRaw, *primaryRoot) ==
              TileOcclusionState::Occluded,
          "Scene: occlusion callback reaches primary tileset");
    check(TilesetTestAccess::checkOcclusion(*additionalRaw, *additionalRoot) ==
              TileOcclusionState::Occluded,
          "Scene: occlusion callback reaches additional tileset");

    scene.setOcclusionCallback(
        [](const TilesetTile&) { return TileOcclusionState::NotOccluded; });
    check(TilesetTestAccess::checkOcclusion(*primaryRaw, *primaryRoot) ==
              TileOcclusionState::NotOccluded,
          "Scene: occlusion callback update reaches primary tileset");
    check(TilesetTestAccess::checkOcclusion(*additionalRaw, *additionalRoot) ==
              TileOcclusionState::NotOccluded,
          "Scene: occlusion callback update reaches additional tileset");
}

void testSceneAdditionalTilesetRendersGltfWithoutReplacingTerrain() {
    DummyRenderDevice device;
    Scene scene;
    check(scene.setRenderDevice(&device),
          "Scene: dummy render device initializes full renderer path");
    scene.setViewport(800, 600, 1.0f);

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 target(ellipsoid.semiMajorAxis(), 0.0, 0.0);
    scene.camera().lookAt(
        target + Vec3(1000000.0, 0.0, 0.0),
        target,
        Vec3::unitZ());

    const TileKey westRoot{"Geographic-TMS", 0, 0, 0};
    const TileKey eastRoot{"Geographic-TMS", 0, 1, 0};

    auto terrainProvider = std::make_unique<SparseTerrainProvider>();
    auto terrainTileset = std::make_unique<Tileset>(
        std::move(terrainProvider),
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        &device,
        TilesetOptions{});
    Tileset* terrainRaw = terrainTileset.get();
    TilesetTestAccess::ensureTile(*terrainRaw, westRoot);
    TilesetTestAccess::ensureTile(*terrainRaw, eastRoot);
    TilesetTestAccess::putTerrainCache(
        *terrainRaw,
        westRoot,
        makeFlatHeightmap(123.0f));
    TilesetTestAccess::putTerrainCache(
        *terrainRaw,
        eastRoot,
        makeFlatHeightmap(123.0f));

    scene.setTileset(std::move(terrainTileset));
    check(scene.tileset() == terrainRaw,
          "Scene: setTileset installs the primary terrain tileset");
    check(std::abs(scene.tileset()->sampleHeight(0.0, 0.0) - 123.0f) <
              1e-6f,
          "Scene: primary terrain tileset remains the height sampling source");

    auto contentTileset = std::make_unique<Tileset>(
        std::unique_ptr<TerrainProvider>{},
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        &device,
        TilesetOptions{});
    TilesetTile* westContent =
        TilesetTestAccess::ensureTile(*contentTileset, westRoot);
    TilesetTile* eastContent =
        TilesetTestAccess::ensureTile(*contentTileset, eastRoot);
    check(westContent != nullptr && eastContent != nullptr,
          "Scene: additional glTF tileset roots are created");
    if (!westContent || !eastContent) return;

    westContent->gltfModel = makeTriangleGltfModel();
    westContent->loadState = TileLoadState::Done;
    westContent->contentKind = TileContentKind::Render;
    eastContent->gltfModel = makeTriangleGltfModel();
    eastContent->loadState = TileLoadState::Done;
    eastContent->contentKind = TileContentKind::Render;

    scene.addTileset(std::move(contentTileset));
    check(scene.tileset() == terrainRaw &&
              scene.additionalTilesetCount() == 1,
          "Scene: addTileset keeps glTF content separate from primary terrain");
    check(std::abs(scene.tileset()->sampleHeight(0.0, 0.0) - 123.0f) <
              1e-6f,
          "Scene: adding glTF tileset does not replace terrain sampling");

    scene.update(1.0 / 60.0);
    scene.render();

    const bool submittedGltf = std::any_of(
        device.submittedCommands.begin(),
        device.submittedCommands.end(),
        [](const RenderCommand& cmd) {
            return cmd.kind == RenderCommandKind::GltfPrimitive;
        });
    check(submittedGltf,
          "Scene: additional glTF tileset contributes GltfPrimitive commands");
    check(scene.diagnostics().renderGltfPrimitives > 0,
          "Scene: diagnostics expose rendered glTF primitive count");
    check(scene.diagnostics().contentTilesets == 1 &&
              scene.diagnostics().contentVisibleTiles > 0,
          "Scene: diagnostics expose additional content tileset visibility");
    check(std::abs(scene.tileset()->sampleHeight(0.0, 0.0) - 123.0f) <
              1e-6f,
          "Scene: terrain sampling is still owned by primary tileset after render");
}

void testTilesetLodTransitionsUseNativeDeltaState() {
    TilesetOptions options;
    options.enableLodTransitionPeriod = true;
    options.lodTransitionLength = 1.0f;

    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::move(provider),
        std::move(scheme),
        {},
        nullptr,
        options);

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: LOD transition root tile is created");
    if (!root) return;

    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(0.0f));
    TilesetTestAccess::ensureTileMesh(tileset, *root);

    TilesetTestAccess::beginTilePlan(tileset);
    root->previousSelectionState = TileSelectionState::NotVisited;
    TilesetTestAccess::renderSelectedTile(tileset, *root);
    TilesetTestAccess::updateLodTransitions(tileset, 0.25);
    check(std::abs(root->lodTransitionFadePercentage - 0.25f) < 1e-6f,
          "Tileset: selected tile fades in by delta / transition length");
    check(TilesetTestAccess::tilePlan(tileset).fadingNodeCount == 1,
          "Tileset: fade-in contributes to transition diagnostics");

    TilesetTestAccess::beginTilePlan(tileset);
    root->selectionState = TileSelectionState::NotVisited;
    root->previousSelectionState = TileSelectionState::Rendered;
    TilesetTestAccess::updateLodTransitions(tileset, 0.25);
    const TilePlan& fadeOutPlan = TilesetTestAccess::tilePlan(tileset);
    check(fadeOutPlan.visibleTiles.empty() &&
              fadeOutPlan.tilesFadingOut.size() == 1,
          "Tileset: previously rendered tile leaves selection through tilesFadingOut");
    check(!fadeOutPlan.tilesFadingOut.empty() &&
              std::abs(fadeOutPlan.tilesFadingOut.front().opacity - 0.75f) < 1e-6f,
          "Tileset: fading-out render opacity is the inverse native fade percentage");
    check(TilesetTestAccess::fadingOutSetSize(tileset) == 1,
          "Tileset: fading-out tile stays tracked across frames");

    TilesetTestAccess::beginTilePlan(tileset);
    root->previousSelectionState = TileSelectionState::NotVisited;
    TilesetTestAccess::updateLodTransitions(tileset, 0.75);
    check(TilesetTestAccess::fadingOutSetSize(tileset) == 1 &&
              !TilesetTestAccess::tilePlan(tileset).tilesFadingOut.empty() &&
              TilesetTestAccess::tilePlan(tileset).tilesFadingOut.front().opacity <= 0.001f,
          "Tileset: fading-out tile reaches zero opacity before removal");

    TilesetTestAccess::beginTilePlan(tileset);
    TilesetTestAccess::updateLodTransitions(tileset, 0.016);
    check(TilesetTestAccess::fadingOutSetSize(tileset) == 0,
          "Tileset: completed fading-out tile is removed like cesium-native");
}

void testTilesetLodTransitionsIgnoreEmptyContent() {
    TilesetOptions options;
    options.enableLodTransitionPeriod = true;
    options.lodTransitionLength = 1.0f;

    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::move(provider),
        std::move(scheme),
        {},
        nullptr,
        options);

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: empty-content LOD transition root tile is created");
    if (!root) return;

    root->loadState = TileLoadState::Done;
    root->contentKind = TileContentKind::Empty;
    root->meshReady = false;

    TilesetTestAccess::beginTilePlan(tileset);
    root->previousSelectionState = TileSelectionState::NotVisited;
    TilesetTestAccess::renderSelectedTile(tileset, *root);
    TilesetTestAccess::updateLodTransitions(tileset, 0.25);
    check(TilesetTestAccess::tilePlan(tileset).tileTransitions.empty() &&
              TilesetTestAccess::tilePlan(tileset).fadingNodeCount == 0 &&
              std::abs(root->lodTransitionFadePercentage - 1.0f) < 1e-6f,
          "Tileset: Empty content does not create fake fade-in render content");

    TilesetTestAccess::beginTilePlan(tileset);
    root->selectionState = TileSelectionState::NotVisited;
    root->previousSelectionState = TileSelectionState::Rendered;
    TilesetTestAccess::updateLodTransitions(tileset, 0.25);
    check(TilesetTestAccess::tilePlan(tileset).tilesFadingOut.empty() &&
              TilesetTestAccess::fadingOutSetSize(tileset) == 0,
          "Tileset: Empty content does not create fake fade-out render content");
}

void testTilesetOcclusionStopsRefinementBeforeChildLoads() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: occlusion root tile is created");
    if (!root) return;

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(center + center.normalized() * 1000000.0,
                  center,
                  Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 101;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::setOcclusionState(
        tileset,
        TileOcclusionState::Occluded);
    TilesetTestAccess::selectTiles(tileset, frameState);

    check(tileset.tilePlan().maxVisibleZoom == 0 && root->children.empty(),
          "Tileset: occluded tile renders current level instead of refining");
    check(tileset.tilePlan().selectionOccludedCount == 1,
          "Tileset: occluded tile increments native occlusion diagnostics");
}

void testTilesetOcclusionUnavailableDelaysNewRefinement() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: occlusion-unavailable root tile is created");
    if (!root) return;

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(center + center.normalized() * 1000000.0,
                  center,
                  Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 102;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::setOcclusionState(
        tileset,
        TileOcclusionState::OcclusionUnavailable);
    TilesetTestAccess::selectTiles(tileset, frameState);

    check(tileset.tilePlan().maxVisibleZoom == 0 && root->children.empty(),
          "Tileset: unavailable occlusion delays new refinement like cesium-native");
    check(tileset.tilePlan().selectionWaitingForOcclusionResultsCount == 1,
          "Tileset: unavailable occlusion increments native waiting diagnostics");
}

void testTilesetOcclusionUnavailableDoesNotDelayPreviouslyRefinedTile() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: previous-refined occlusion root tile is created");
    if (!root) return;

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(center + center.normalized() * 1000000.0,
                  center,
                  Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 103;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    root->selectionState = TileSelectionState::Refined;
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::setOcclusionState(
        tileset,
        TileOcclusionState::OcclusionUnavailable);
    TilesetTestAccess::selectTiles(tileset, frameState);

    check(!root->children.empty() && tileset.tilePlan().maxVisibleZoom > 0,
          "Tileset: previously refined tile continues refinement while occlusion is unavailable");
}

void testTilesetOcclusionAggregatesOccludedChildren() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: child-aggregate occlusion root tile is created");
    if (!root) return;

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    check(!root->children.empty(),
          "Tileset: child-aggregate occlusion has child bounds");
    if (root->children.empty()) return;

    std::unordered_map<TileKey, TileOcclusionState> states;
    states[rootKey] = TileOcclusionState::NotOccluded;
    for (const TilesetTile* child : root->children) {
        states[child->key] = TileOcclusionState::Occluded;
    }
    TilesetTestAccess::setOcclusionCallback(
        tileset,
        [states](const TilesetTile& tile) {
            auto it = states.find(tile.key);
            return it == states.end()
                ? TileOcclusionState::NotOccluded
                : it->second;
        });

    check(TilesetTestAccess::checkOcclusion(tileset, *root) ==
              TileOcclusionState::Occluded,
          "Tileset: parent uses occluded child union like cesium-native");
}

void testTilesetOcclusionVisibleChildKeepsParentRefining() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: visible-child occlusion root tile is created");
    if (!root) return;

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    check(!root->children.empty(),
          "Tileset: visible-child occlusion has child bounds");
    if (root->children.empty()) return;

    std::unordered_map<TileKey, TileOcclusionState> states;
    states[rootKey] = TileOcclusionState::NotOccluded;
    for (const TilesetTile* child : root->children) {
        states[child->key] = TileOcclusionState::Occluded;
    }
    states[root->children.front()->key] = TileOcclusionState::NotOccluded;
    TilesetTestAccess::setOcclusionCallback(
        tileset,
        [states](const TilesetTile& tile) {
            auto it = states.find(tile.key);
            return it == states.end()
                ? TileOcclusionState::NotOccluded
                : it->second;
        });

    check(TilesetTestAccess::checkOcclusion(tileset, *root) ==
              TileOcclusionState::NotOccluded,
          "Tileset: one visible child keeps parent unoccluded");
}

void testTilesetOcclusionUnavailableChildDelaysNewRefinement() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: unavailable-child occlusion root tile is created");
    if (!root) return;

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    check(!root->children.empty(),
          "Tileset: unavailable-child occlusion has child bounds");
    if (root->children.empty()) return;

    std::unordered_map<TileKey, TileOcclusionState> states;
    states[rootKey] = TileOcclusionState::NotOccluded;
    for (const TilesetTile* child : root->children) {
        states[child->key] = TileOcclusionState::Occluded;
    }
    states[root->children.front()->key] =
        TileOcclusionState::OcclusionUnavailable;
    TilesetTestAccess::setOcclusionCallback(
        tileset,
        [states](const TilesetTile& tile) {
            auto it = states.find(tile.key);
            return it == states.end()
                ? TileOcclusionState::NotOccluded
                : it->second;
        });

    const Vec3 center = TilesetTestAccess::tileBoundsCenter(root->bounds);
    Camera camera;
    camera.lookAt(center + center.normalized() * 1000000.0,
                  center,
                  Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 104;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));
    TilesetTestAccess::setLastCamera(
        tileset,
        camera.position(),
        camera.direction());
    TilesetTestAccess::selectTiles(tileset, frameState);

    const bool childrenNotVisited = std::all_of(
        root->children.begin(),
        root->children.end(),
        [](const TilesetTile* child) {
            return child &&
                   child->selectionState == TileSelectionState::NotVisited;
        });
    check(childrenNotVisited &&
              tileset.tilePlan().selectionWaitingForOcclusionResultsCount == 1,
          "Tileset: unavailable child union delays new refinement");
}

void testTilesetOcclusionUnconditionalChildSkipsAggregation() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: unconditional-child occlusion root tile is created");
    if (!root) return;

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    check(!root->children.empty(),
          "Tileset: unconditional-child occlusion has child bounds");
    if (root->children.empty()) return;

    root->children.front()->unconditionallyRefine = true;
    std::unordered_map<TileKey, TileOcclusionState> states;
    states[rootKey] = TileOcclusionState::NotOccluded;
    for (const TilesetTile* child : root->children) {
        states[child->key] = TileOcclusionState::Occluded;
    }
    TilesetTestAccess::setOcclusionCallback(
        tileset,
        [states](const TilesetTile& tile) {
            auto it = states.find(tile.key);
            return it == states.end()
                ? TileOcclusionState::NotOccluded
                : it->second;
        });

    check(TilesetTestAccess::checkOcclusion(tileset, *root) ==
              TileOcclusionState::NotOccluded,
          "Tileset: unconditionally refined child disables child occlusion union");
}

void testTilesetDefaultSoftwareOcclusionCanCullFarSideTile() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey farSideKey{"Geographic-TMS", 2, 7, 1};
    TilesetTile* farSide = TilesetTestAccess::ensureTile(tileset, farSideKey);
    check(farSide != nullptr,
          "Tileset: software-occlusion far-side tile is created");
    if (!farSide) return;

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 cameraPosition(ellipsoid.semiMajorAxis() + 1000000.0, 0.0, 0.0);
    TilesetTestAccess::setLastCamera(
        tileset,
        cameraPosition,
        Vec3(-1.0, 0.0, 0.0));

    check(TilesetTestAccess::checkOcclusion(tileset, *farSide) ==
              TileOcclusionState::Occluded,
          "Tileset: default software ellipsoid occlusion culls far-side tile");
}

void testTilesetChildrenInheritParentTerrainHeightRange() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    const float minimumHeight = -320.0f;
    const float maximumHeight = 2048.0f;
    auto heightmap = makeFlatHeightmap(0.0f);
    heightmap->rawData = makeQuantizedMeshBytes(
        "", false, false, Vec3::zero(), minimumHeight, maximumHeight);
    TilesetTestAccess::putTerrainCache(tileset, rootKey, std::move(heightmap));

    check(root != nullptr,
          "Tileset: height-range inheritance root tile is created");
    if (!root) return;

    TilesetTestAccess::ensureTileMesh(tileset, *root);
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    check(root->children.size() == 4,
          "Tileset: height-range inheritance creates child tiles");

    bool allChildrenInherited = !root->children.empty();
    for (const TilesetTile* child : root->children) {
        allChildrenInherited =
            allChildrenInherited &&
            child &&
            child->hasTerrainHeightRange &&
            std::abs(child->terrainMinimumHeight - minimumHeight) < 1e-6 &&
            std::abs(child->terrainMaximumHeight - maximumHeight) < 1e-6;
    }
    check(allChildrenInherited,
          "Tileset: child terrain bounds inherit parent QM updated height range");
}

void testTilesetSampleHeightUsesBestLoadedTerrainTile() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};
    TilesetTestAccess::ensureTile(tileset, rootKey);
    TilesetTestAccess::ensureTile(tileset, childKey);
    TilesetTestAccess::putTerrainCache(
        tileset, rootKey, makeFlatHeightmap(10.0f));
    TilesetTestAccess::putTerrainCache(
        tileset, childKey, makeFlatHeightmap(42.0f));

    const Rectangle childBounds = tileset.tileScheme().tileToRectangle(childKey);
    const double lng = (childBounds.west() + childBounds.east()) * 0.5;
    const double lat = (childBounds.south() + childBounds.north()) * 0.5;

    check(std::abs(tileset.sampleHeight(lng, lat) - 42.0f) < 1e-6f,
          "Tileset: sampleHeight uses the most detailed loaded terrain tile");
}

void testTilesetSampleHeightFallsBackToLoadedAncestorTerrain() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};
    TilesetTestAccess::ensureTile(tileset, rootKey);
    TilesetTestAccess::ensureTile(tileset, childKey);
    TilesetTestAccess::putTerrainCache(
        tileset, rootKey, makeFlatHeightmap(123.0f));

    const Rectangle childBounds = tileset.tileScheme().tileToRectangle(childKey);
    const double lng = (childBounds.west() + childBounds.east()) * 0.5;
    const double lat = (childBounds.south() + childBounds.north()) * 0.5;

    check(std::abs(tileset.sampleHeight(lng, lat) - 123.0f) < 1e-6f,
          "Tileset: sampleHeight falls back to a loaded ancestor terrain tile");
}

void testTilesetCreatesUpsampledChildrenForUnavailableSiblings() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    SparseTerrainProvider* rawProvider = provider.get();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr, "Tileset: root tile is created for sparse provider");
    TilesetTestAccess::putTerrainCache(
        tileset, rootKey, makeFlatHeightmap(123.0f));
    TilesetTestAccess::ensureTileMesh(tileset, *root);
    check(root->meshReady && root->mesh != nullptr,
          "Tileset: parent render mesh is ready before upsample children");

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    check(root->children.size() == 4,
          "Tileset: partial availability creates all four children");

    TilesetTile* sw = root->children[0];
    TilesetTile* se = root->children[1];
    TilesetTile* nw = root->children[2];
    TilesetTile* ne = root->children[3];
    check(sw && !sw->upsampledFromParent,
          "Tileset: available child remains a requestable real tile");
    check(se && se->upsampledFromParent &&
              nw && nw->upsampledFromParent &&
              ne && ne->upsampledFromParent,
          "Tileset: unavailable siblings become upsampled children");
    check(sw && se &&
              std::abs(sw->geometricError - root->geometricError * 0.5) < 1e-9 &&
              std::abs(se->geometricError - root->geometricError * 0.5) < 1e-9,
          "Tileset: child geometric error is parent error halved like cesium-native");
    check(se && !TilesetTestAccess::isTileRenderable(tileset, *se),
          "Tileset: upsampled child waits for main-thread upsample finish before becoming renderable");

    TilesetTestAccess::requestMissingTile(tileset, se->key);
    check(se->loadState == TileLoadState::ContentLoading &&
              tileset.loadDiagnostics().pendingTerrainUploads == 1 &&
              rawProvider->requestCount == 0,
          "Tileset: upsampled child enters local main-thread queue without terrain provider request");

    TilesetTestAccess::processPendingUploads(tileset);
    check(se->meshReady && se->mesh != nullptr && !se->mesh->vertices.empty(),
          "Tileset: upsampled child builds mesh from parent render mesh");
    check(se->loadState == TileLoadState::Done &&
              TilesetTestAccess::isTileRenderable(tileset, *se),
          "Tileset: upsampled child is renderable after parent-mesh upsample finishes");
}

void testTilesetUpsampledChildQueuesParentUntilSourceReady() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    SparseTerrainProvider* rawProvider = provider.get();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: parent-wait upsample root tile is created");
    if (!root) return;

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    check(root->children.size() == 4,
          "Tileset: parent-wait setup creates partial-availability children");
    if (root->children.size() < 2 || !root->children[1]) return;

    TilesetTile* upsampledChild = root->children[1];
    check(upsampledChild->upsampledFromParent,
          "Tileset: parent-wait child is marked as upsampled terrain");

    TilesetTestAccess::requestMissingTile(tileset, upsampledChild->key);
    const TilesetLoadDiagnostics diag = tileset.loadDiagnostics();
    check(upsampledChild->loadState == TileLoadState::Unloaded &&
              diag.pendingTerrainUploads == 0 &&
              rawProvider->requestCount == 0,
          "Tileset: upsampled child waits without requesting missing terrain");
    check(diag.loadQueueUrgentRequests == 1 &&
              diag.loadQueueNormalRequests == 0,
          "Tileset: upsampled child queues its parent urgently until a source mesh is ready");
}

void testTilesetUpsampledChildFinalizesContentLoadedParent() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    SparseTerrainProvider* rawProvider = provider.get();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: content-loaded-parent upsample root tile is created");
    if (!root) return;

    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(33.0f));
    root->loadState = TileLoadState::ContentLoaded;
    root->contentKind = TileContentKind::Render;

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    check(root->children.size() == 4,
          "Tileset: content-loaded-parent setup creates partial-availability children");
    if (root->children.size() < 2 || !root->children[1]) return;

    TilesetTile* upsampledChild = root->children[1];
    check(upsampledChild->upsampledFromParent,
          "Tileset: content-loaded-parent child is marked as upsampled terrain");

    TilesetTestAccess::requestMissingTile(tileset, upsampledChild->key);
    check(root->loadState == TileLoadState::Done &&
              root->meshReady &&
              upsampledChild->loadState == TileLoadState::ContentLoading &&
              tileset.loadDiagnostics().pendingTerrainUploads == 1 &&
              rawProvider->requestCount == 0,
          "Tileset: upsampled child finalizes ContentLoaded parent before local upsample");

    TilesetTestAccess::processPendingUploads(tileset);
    check(upsampledChild->loadState == TileLoadState::Done &&
              upsampledChild->meshReady &&
              upsampledChild->mesh != nullptr &&
              TilesetTestAccess::isTileRenderable(tileset, *upsampledChild),
          "Tileset: upsampled child becomes renderable after finalized-parent upsample");
}

void testTilesetClearChildrenErasesFlatMapDescendants() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: clear-children root tile is created");
    if (!root) return;

    TilesetTestAccess::putTerrainCache(
        tileset, rootKey, makeFlatHeightmap(1.0f));
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    check(root->children.size() == 4,
          "Tileset: clear-children setup creates child tiles");

    const TileKey childKey = root->children[1]->key;
    TilesetTestAccess::clearChildrenRecursively(tileset, *root);
    check(root->children.empty(),
          "Tileset: clear-children empties parent child list");
    check(TilesetTestAccess::findTile(tileset, childKey) == nullptr,
          "Tileset: clear-children removes descendants from flat tile map");

    TilesetTile* recreated = TilesetTestAccess::ensureTile(tileset, childKey);
    check(recreated && recreated->parent == root,
          "Tileset: recreated child is relinked to parent");
    check(root->children.size() == 1 && root->children.front() == recreated,
          "Tileset: recreated child is present in parent child list");
}

void testTilesetClearChildrenIgnoresStaleTerrainCallback() {
    auto provider = std::make_unique<ManualCompletionTerrainProvider>();
    ManualCompletionTerrainProvider* rawProvider = provider.get();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: stale terrain callback root tile is created");
    if (!root) return;

    TilesetTestAccess::putTerrainCache(
        tileset, rootKey, makeFlatHeightmap(1.0f));
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    check(!root->children.empty(),
          "Tileset: stale terrain callback setup creates child");
    if (root->children.empty()) return;

    const TileKey childKey = root->children.front()->key;
    TilesetTestAccess::requestMissingTile(tileset, childKey);
    check(rawProvider->pendingRequests.size() == 1,
          "Tileset: stale terrain callback has one pending request");

    TilesetTestAccess::clearChildrenRecursively(tileset, *root);
    check(TilesetTestAccess::findTile(tileset, childKey) == nullptr,
          "Tileset: stale terrain callback child is removed before callback");

    check(rawProvider->completeWithHeightmap(childKey, makeFlatHeightmap(2.0f)),
          "Tileset: stale terrain callback can still arrive after clear");
    TilesetTestAccess::processPendingUploads(tileset);

    check(TilesetTestAccess::findTile(tileset, childKey) == nullptr,
          "Tileset: stale terrain callback does not recreate cleared child");
    const TilesetLoadDiagnostics terrainDiag = tileset.loadDiagnostics();
    check(terrainDiag.pendingTerrainTotal() == 0 &&
              terrainDiag.pendingContentTotal() == 0,
          "Tileset: stale terrain callback drains pending lifecycle queues");
}

void testTilesetClearChildrenIgnoresStaleContentCallback() {
    const TileKey contentKey{"Geographic-TMS", 1, 0, 0};
    auto provider = std::make_unique<ManualCompletionContentProvider>(
        contentKey);
    ManualCompletionContentProvider* rawProvider = provider.get();
    DummyRenderDevice device;
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(
        std::unique_ptr<TerrainProvider>{},
        std::move(scheme),
        {},
        &device,
        TilesetOptions{},
        std::move(provider));

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    TilesetTile* child = TilesetTestAccess::ensureTile(tileset, contentKey);
    check(root && child && child->parent == root,
          "Tileset: stale content callback setup links child");
    if (!root || !child) return;

    TilesetTestAccess::requestMissingTile(tileset, contentKey);
    check(rawProvider->pendingRequests.size() == 1,
          "Tileset: stale content callback has one pending request");

    TilesetTestAccess::clearChildrenRecursively(tileset, *root);
    check(TilesetTestAccess::findTile(tileset, contentKey) == nullptr,
          "Tileset: stale content callback child is removed before callback");

    check(rawProvider->completeWithModel(contentKey, makeTriangleGltfModel()),
          "Tileset: stale content callback can still arrive after clear");
    TilesetTestAccess::processPendingUploads(tileset);

    check(TilesetTestAccess::findTile(tileset, contentKey) == nullptr,
          "Tileset: stale content callback does not recreate cleared child");
    const TilesetLoadDiagnostics contentDiag = tileset.loadDiagnostics();
    check(contentDiag.pendingTerrainTotal() == 0 &&
              contentDiag.pendingContentTotal() == 0,
          "Tileset: stale content callback drains pending lifecycle queues");
}

void testTilesetUnloadKeepsParentWithReferencedDescendant() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: referenced-descendant unload root tile is created");
    if (!root) return;

    auto heightmap = makeFlatHeightmap(1.0f);
    heightmap->rawData.resize(128, 7);
    TilesetTestAccess::putTerrainCache(tileset, rootKey, std::move(heightmap));
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::Done;
    root->meshReady = true;
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    check(!root->children.empty(),
          "Tileset: referenced-descendant unload setup creates children");
    if (root->children.empty() || !root->children.front()) return;

    const TileKey childKey = root->children.front()->key;
    root->children.front()->addReference();
    TilesetTestAccess::markEligibleForUnloading(tileset, rootKey);
    TilesetTestAccess::updateTotalBytesUsed(tileset);
    TilesetTestAccess::unloadCachedBytes(tileset, 0);

    check(TilesetTestAccess::findTile(tileset, rootKey) == root,
          "Tileset: unload skips parent while descendant is referenced");
    check(TilesetTestAccess::findTile(tileset, childKey) != nullptr,
          "Tileset: unload preserves referenced descendant subtree");
}

void testTilesetUnloadQueueIgnoresUnloadedUnknownTiles() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: unload-queue filter root tile is created");
    if (!root) return;

    TilesetTestAccess::markEligibleForUnloading(tileset, rootKey);
    check(tileset.loadDiagnostics().unloadQueueTiles == 0,
          "Tileset: unloaded unknown tile is not queued for content unloading");

    root->contentKind = TileContentKind::Empty;
    root->loadState = TileLoadState::Done;
    TilesetTestAccess::markEligibleForUnloading(tileset, rootKey);
    check(tileset.loadDiagnostics().unloadQueueTiles == 1,
          "Tileset: loaded empty content remains eligible for content unloading");
}

void testTilesetUnloadRenderContentIgnoresIndependentChildLoading() {
    auto provider = std::make_unique<ManualCompletionTerrainProvider>();
    ManualCompletionTerrainProvider* rawProvider = provider.get();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: independent-child-loading unload root tile is created");
    if (!root) return;

    auto rootHeightmap = makeFlatHeightmap(1.0f);
    rootHeightmap->rawData.resize(96, 1);
    TilesetTestAccess::putTerrainCache(
        tileset, rootKey, std::move(rootHeightmap));
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::Done;
    root->meshReady = true;
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    check(!root->children.empty(),
          "Tileset: independent-child-loading setup creates children");
    if (root->children.empty() || !root->children.front()) return;

    const TileKey childKey = root->children.front()->key;
    TilesetTestAccess::requestMissingTile(tileset, childKey);
    check(rawProvider->pendingRequests.size() == 1,
          "Tileset: independent child has its own pending terrain request");

    TilesetTestAccess::markEligibleForUnloading(tileset, rootKey);
    TilesetTestAccess::updateTotalBytesUsed(tileset);
    TilesetTestAccess::unloadCachedBytes(tileset, 0);

    TilesetTile* rootAfter = TilesetTestAccess::findTile(tileset, rootKey);
    TilesetTile* childAfter = TilesetTestAccess::findTile(tileset, childKey);
    check(rootAfter == root &&
              rootAfter->loadState == TileLoadState::Unloaded &&
              rootAfter->contentKind == TileContentKind::Unknown,
          "Tileset: independent child loading does not retain parent render content");
    check(childAfter &&
              childAfter->loadState == TileLoadState::ContentLoading &&
              rawProvider->pendingRequests.size() == 1,
          "Tileset: independent child loading remains pending after parent content unload");

    check(rawProvider->completeWithHeightmap(childKey, makeFlatHeightmap(2.0f)),
          "Tileset: independent child pending request is completed for cleanup");
    TilesetTestAccess::processPendingUploads(tileset);
}

void testTilesetUnloadRenderContentWaitsForUpsampledChildLoading() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: upsample-child-loading unload root tile is created");
    if (!root) return;

    TilesetTestAccess::putTerrainCache(
        tileset, rootKey, makeFlatHeightmap(1.0f));
    TilesetTestAccess::ensureTileMesh(tileset, *root);
    check(root->loadState == TileLoadState::Done &&
              root->contentKind == TileContentKind::Render &&
              root->meshReady &&
              root->mesh != nullptr,
          "Tileset: upsample-child-loading parent source mesh is ready");
    root->gpuVertexBuffer = std::make_unique<DummyBuffer>(256);
    root->gpuIndexBuffer = std::make_unique<DummyBuffer>(96);
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    check(!root->children.empty(),
          "Tileset: upsample-child-loading setup creates children");
    if (root->children.empty() || !root->children.front()) return;

    TilesetTile* child = root->children.front();
    child->upsampledFromParent = true;
    TilesetTestAccess::requestMissingTile(tileset, child->key);
    check(child->loadState == TileLoadState::ContentLoading &&
              tileset.loadDiagnostics().pendingTerrainUploads == 1,
          "Tileset: upsample child has a pending main-thread upload before parent unload");

    TilesetTestAccess::markEligibleForUnloading(tileset, rootKey);
    TilesetTestAccess::updateTotalBytesUsed(tileset);
    const int64_t bytesBeforeUnload = tileset.totalBytesUsed();
    TilesetTestAccess::unloadCachedBytes(tileset, 0);

    check(root->loadState == TileLoadState::Unloading &&
              root->contentKind == TileContentKind::Render &&
              root->meshReady &&
              root->mesh != nullptr,
          "Tileset: upsample child loading moves parent into Unloading while keeping source content");
    check(root->gpuVertexBuffer == nullptr &&
              root->gpuIndexBuffer == nullptr,
          "Tileset: upsample-protected parent releases main-thread render resources first");
    check(tileset.totalBytesUsed() < bytesBeforeUnload,
          "Tileset: partial Unloading updates byte accounting after render-resource release");
    check(tileset.loadDiagnostics().unloadQueueTiles == 1,
          "Tileset: upsample-protected parent remains queued for a later unload retry");

    TilesetTestAccess::processPendingUploads(tileset);
    check(child->loadState == TileLoadState::Done &&
              child->contentKind == TileContentKind::Render &&
              child->meshReady &&
              child->mesh != nullptr,
          "Tileset: pending upsample child can finish from an Unloading parent source");
    TilesetTestAccess::unloadCachedBytes(tileset, 0);

    check(root->loadState == TileLoadState::Unloaded &&
              root->contentKind == TileContentKind::Unknown &&
              !root->meshReady &&
              root->mesh == nullptr,
          "Tileset: upsample-protected parent finishes unloading after child load leaves ContentLoading");
    check(tileset.loadDiagnostics().unloadQueueTiles == 0,
          "Tileset: finished Unloading parent is removed from the unload queue");
}

void testTilesetUnloadRenderContentPreservesLoadedChildren() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: render-content unload-preserve root tile is created");
    if (!root) return;

    auto rootHeightmap = makeFlatHeightmap(1.0f);
    rootHeightmap->rawData.resize(96, 1);
    TilesetTestAccess::putTerrainCache(
        tileset, rootKey, std::move(rootHeightmap));
    root->contentKind = TileContentKind::Render;
    root->loadState = TileLoadState::Done;
    root->meshReady = true;

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    check(!root->children.empty(),
          "Tileset: render-content unload-preserve setup creates children");
    if (root->children.empty() || !root->children.front()) return;

    TilesetTile* child = root->children.front();
    const TileKey childKey = child->key;
    auto childHeightmap = makeFlatHeightmap(2.0f);
    childHeightmap->rawData.resize(96, 2);
    TilesetTestAccess::putTerrainCache(
        tileset, childKey, std::move(childHeightmap));
    child->contentKind = TileContentKind::Render;
    child->loadState = TileLoadState::Done;
    child->meshReady = true;

    TilesetTestAccess::markEligibleForUnloading(tileset, rootKey);
    TilesetTestAccess::updateTotalBytesUsed(tileset);
    TilesetTestAccess::unloadCachedBytes(tileset, 0);

    TilesetTile* rootAfter = TilesetTestAccess::findTile(tileset, rootKey);
    TilesetTile* childAfter = TilesetTestAccess::findTile(tileset, childKey);
    check(rootAfter == root &&
              rootAfter->loadState == TileLoadState::Unloaded &&
              rootAfter->contentKind == TileContentKind::Unknown &&
              !rootAfter->meshReady,
          "Tileset: cache unload removes only the render parent's content");
    check(childAfter == child &&
              childAfter->loadState == TileLoadState::Done &&
              childAfter->contentKind == TileContentKind::Render &&
              childAfter->parent == rootAfter,
          "Tileset: render-content cache unload preserves loaded children");
}

void testTilesetUnloadExternalContentClearsChildren() {
    auto provider = std::make_unique<SparseTerrainProvider>();
    auto scheme = TileScheme::createGeographicTMS();
    Tileset tileset(std::move(provider), std::move(scheme), {}, nullptr, TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    check(root != nullptr,
          "Tileset: external-content unload root tile is created");
    if (!root) return;

    root->contentKind = TileContentKind::External;
    root->loadState = TileLoadState::Done;
    root->unconditionallyRefine = true;
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    check(!root->children.empty(),
          "Tileset: external-content unload setup creates children");
    if (root->children.empty() || !root->children.front()) return;

    const TileKey childKey = root->children.front()->key;
    auto childHeightmap = makeFlatHeightmap(3.0f);
    childHeightmap->rawData.resize(96, 3);
    TilesetTestAccess::putTerrainCache(
        tileset, childKey, std::move(childHeightmap));

    TilesetTestAccess::markEligibleForUnloading(tileset, rootKey);
    TilesetTestAccess::updateTotalBytesUsed(tileset);
    TilesetTestAccess::unloadCachedBytes(tileset, 0);

    TilesetTile* rootAfter = TilesetTestAccess::findTile(tileset, rootKey);
    check(rootAfter == root &&
              rootAfter->children.empty() &&
              rootAfter->loadState == TileLoadState::Unloaded &&
              rootAfter->contentKind == TileContentKind::Unknown,
          "Tileset: external-content cache unload clears wrapper children");
    check(TilesetTestAccess::findTile(tileset, childKey) == nullptr,
          "Tileset: external-content cache unload removes descendants from flat map");
}

} // namespace

int main() {
    std::cout << "=== Engine Regression Tests ===\n\n";
    testTileSelectionKickedStatePreservesOriginalResult();
    testEllipsoidScaleToGeodeticSurfaceMatchesCesiumNative();
    testEllipsoidRayIntersectionIntervalMatchesCesiumNative();
    testTransformsEastNorthUpToFixedFrameMatchesCesiumNative();
    testOrientedBoundingBoxDegenerateAxesMatchCesiumNative();
    testBoundingSpherePlaneBoundaryMatchesCesiumNative();
    testTilesetMaximumCachedBytesMatchesCesiumNativeDefault();
    testTilesetByteEstimateUsesActualMeshPayload();
    testRasterOverlayProviderRetention();
    testActivatedRasterOverlayBindsProviderOwner();
    testRasterOverlayProviderRectangleTile();
    testRasterMappedUsesRenderContentDetailsRectangle();
    testRasterMappedMissingProjectionUsesPlaceholder();
    testTilesetMissingRasterProjectionUnloadsRenderContent();
    testTilesetRasterTargetPixelsUseRenderContentRectangle();
    testTilesetGltfRenderContentBuildsPrimitiveCommands();
    testTilesetGltfTangentsUseModelLinearTransform();
    testTilesetGltfMaskMaterialStaysOpaqueCommand();
    testTilesetGltfUnlitMaterialUploadsUniform();
    testTilesetGltfEmissiveMaterialUploadsUniformsAndTextures();
    testTilesetGltfIorMaterialUploadsSpecularF0();
    testTilesetGltfAnisotropyMaterialUploadsUniformsAndTextures();
    testTilesetGltfPbrSpecularGlossinessUploadsUniformsAndTextures();
    testTilesetGltfTransmissionMaterialUploadsUniformsAndTextures();
    testTilesetGltfSpecularMaterialUploadsUniformsAndTextures();
    testTilesetGltfClearcoatMaterialUploadsUniformsAndTextures();
    testTilesetGltfSheenMaterialUploadsUniformsAndTextures();
    testTilesetGltfPointAndLineModesReachDrawCommands();
    testTilesetGltfDoubleSidedDisablesCullOnly();
    testTilesetGltfOpaqueInstancesUseGpuInstancing();
    testTilesetGltfBlendInstancesSplitForSorting();
    testTilesetGltfTransmissionInstancesSplitForSorting();
    testTilesetContentProviderLoadsGltfRenderContent();
    testTilesetContentProviderLoadsNativeGpuInstancingGltf();
    testTilesetJsonProviderLoadsExplicitGltfTile();
    testTilesetJsonGltfUpAxisZKeepsZUpContent();
    testTilesetJsonI3dmDefaultUpAxisKeepsInstancePositionsTileLocal();
    testTilesetJsonUnsupportedMultipleContentsFailsTile();
    testTilesetJsonProviderParsesViewerRequestVolume();
    testTilesetJsonTopLevelUnknownRequiredExtensionInvalidatesProvider();
    testTilesetJsonUnsupportedTileRequiredExtensionFailsTile();
    testTilesetJsonUnsupportedImplicitTilingFailsTile();
    testTilesetJsonProviderLoadsExternalTilesetContent();
    testTilesetViewerRequestVolumeGatesContentLoadQueue();
    testTilesetUnloadRenderContentReleasesGltfResources();
    testRasterMappedAttachedUnknownReportsMoreDetail();
    testRasterMappedFailureFallbackMatchesOverlayOwner();
    testRasterOverlayNativeTranslationAndRendererWindow();
    testQuantizedMeshAvailabilityLevels();
    testQuantizedMeshAvailabilityInclusiveCenterBoundary();
    testQuantizedMeshMetadataAvailabilityStartsAtRoots();
    testQuantizedMeshLayerJsonEmptyAvailabilityMatchesCesiumNative();
    testQuantizedMeshRejectsOutOfRangeGeographicTiles();
    testQuantizedMeshLayerJsonMinzoomDoesNotGateAvailability();
    testQuantizedMeshLayerJsonMaxzoomDoesNotGateExplicitAvailability();
    testQuantizedMeshLayerJsonAvailabilityUint32Defaults();
    testQuantizedMeshMetadataExtensionLengthPrefixMatchesCesiumNative();
    testQuantizedMeshSkirtNormalsCopyEdgeNormals();
    testQuantizedMeshRtcOriginFromBoundingSphereCenter();
    testQuantizedMeshHeaderHeightRangeIsExposed();
    testTilesetUsesQuantizedMeshRtcOrigin();
    testTilesetUsesQuantizedMeshHeightRange();
    testTilesetBoundsUseQuantizedMeshHeightRange();
    testTilesetBoundingRegionObbUsesQuantizedMeshHeightRange();
    testTilesetBoundingRegionObbHandlesLargeRectanglesLikeCesiumNative();
    testQuantizedMeshLayerJsonVersionAndExtensionQuery();
    testQuantizedMeshLayerJsonUriResolution();
    testQuantizedMeshFabdemLayerJsonShape();
    testQuantizedMeshParentLayerFallback();
    testQuantizedMeshLoadsUnderlyingLayerAvailabilityWithTile();
    testTilesetDefersAvailabilityBoundaryChildrenUntilContentLoaded();
    testTilesetTotalBytesIncludesDecodedHeightmapPayload();
    testTerrainProviderDefaultAvailabilityHonorsZoomRange();
    testTilesetCanRefineQuantizedMeshPastLayerMaxzoomWhenAvailable();
    testTilesetRequestsExplicitAvailabilityPastProviderMaxzoom();
    testTilesetRetryLaterRemainsRetryable();
    testTilesetFailedTerminalDoesNotRetry();
    testTilesetEmptyContentReachesDone();
    testTilesetRenderContentRequiresDoneState();
    testTilesetMappedRasterMustBeReadyForRenderability();
    testTilesetUnconditionallyRefineRenderableOnlyWithoutChildren();
    testTilesetMainThreadLoadingTimeLimitZeroDrainsPendingUploads();
    testTilesetMainThreadPendingUploadsUseNativePriority();
    testTilesetLoadQueueKeepsTraversalPriority();
    testTilesetMainThreadUploadBudgetIsGlobalAcrossContentKinds();
    testTilesetPendingMainThreadUploadsDoNotConsumeWorkerSlots();
    testTilesetDestructorWaitsForPendingTerrainCallbacks();
    testTilesetForbidHolesCarriesCulledTileRenderability();
    testTilesetCulledTileDetailsIgnorePreloadOnlyTiles();
    testTilesetLoadDiagnosticsExposeNativeLifecycleStates();
    testTilesetAdditiveRefinementRendersParentAndChildren();
    testTilesetAdditiveRefinementCullsWithParentBounds();
    testTilesetAdditiveRefinedTileCountsAsPreviouslyRendered();
    testTilesetUnconditionallyRefineIgnoresSatisfiedSse();
    testTilesetUnconditionalChildDisablesChildrenBoundsCulling();
    testTilesetFogDensityTableIsConfigurable();
    testTilesetRecordsAncestorMeetsSseForDescendants();
    testTilesetUsesLargestSseAcrossSelectorViews();
    testTilesetFogUsesEachSelectorViewDensity();
    testSceneSelectorViewOverrideFeedsMultipleViews();
    testSceneOcclusionCallbackFeedsPrimaryAndAdditionalTilesets();
    testSceneAdditionalTilesetRendersGltfWithoutReplacingTerrain();
    testTilesetLodTransitionsUseNativeDeltaState();
    testTilesetLodTransitionsIgnoreEmptyContent();
    testTilesetOcclusionStopsRefinementBeforeChildLoads();
    testTilesetOcclusionUnavailableDelaysNewRefinement();
    testTilesetOcclusionUnavailableDoesNotDelayPreviouslyRefinedTile();
    testTilesetOcclusionAggregatesOccludedChildren();
    testTilesetOcclusionVisibleChildKeepsParentRefining();
    testTilesetOcclusionUnavailableChildDelaysNewRefinement();
    testTilesetOcclusionUnconditionalChildSkipsAggregation();
    testTilesetDefaultSoftwareOcclusionCanCullFarSideTile();
    testTilesetChildrenInheritParentTerrainHeightRange();
    testTilesetSampleHeightUsesBestLoadedTerrainTile();
    testTilesetSampleHeightFallsBackToLoadedAncestorTerrain();
    testTilesetCreatesUpsampledChildrenForUnavailableSiblings();
    testTilesetUpsampledChildQueuesParentUntilSourceReady();
    testTilesetUpsampledChildFinalizesContentLoadedParent();
    testTilesetClearChildrenErasesFlatMapDescendants();
    testTilesetClearChildrenIgnoresStaleTerrainCallback();
    testTilesetClearChildrenIgnoresStaleContentCallback();
    testTilesetUnloadKeepsParentWithReferencedDescendant();
    testTilesetUnloadQueueIgnoresUnloadedUnknownTiles();
    testTilesetUnloadRenderContentIgnoresIndependentChildLoading();
    testTilesetUnloadRenderContentWaitsForUpsampledChildLoading();
    testTilesetUnloadRenderContentPreservesLoadedChildren();
    testTilesetUnloadExternalContentClearsChildren();

    std::cout << "\n=== " << gFailures << " failures ===\n";
    return gFailures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
