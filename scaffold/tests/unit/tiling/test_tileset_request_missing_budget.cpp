#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Projection.h"
#include "earth_engine/core/resources/FrameResourceBudget.h"
#include "earth_engine/platform/bridge/PlatformBridge.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/RasterOverlayTileProvider.h"
#include "earth_engine/providers/RasterOverlayTile.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/renderer/IPrepareRendererResources.h"
#include "earth_engine/renderer/RenderDevice.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/scene/SceneTilesetDiagnostics.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileSelectionRasterOverlayPreparer.h"
#include "earth_engine/tiling/TileSelectionPlanAppender.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"
#include "earth_engine/tiling/TilesetUpdateFrameRuntime.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace earth_engine;

namespace earth_engine {
struct TilesetTestAccess {
    static Tileset makeLegacyTerrainTileset(
        std::unique_ptr<TerrainProvider> terrainProvider,
        std::unique_ptr<TileScheme> scheme,
        std::vector<ActivatedRasterOverlay*> overlays = {},
        RenderDevice* device = nullptr,
        TilesetOptions options = {}) {
        return Tileset::createLegacyTerrainForTests(
            std::move(terrainProvider),
            std::move(scheme),
            std::move(overlays),
            device,
            std::move(options));
    }

    static TilesetTile* ensureTile(Tileset& tileset, const TileKey& key) {
        return tileset.contentAccess_.ensureTile(key);
    }

    static void ensureTileChildren(Tileset& tileset, TilesetTile& tile) {
        tileset.contentAccess_.ensureTileChildren(tile);
    }

    static void ensureTileMesh(Tileset& tileset, TilesetTile& tile) {
        tileset.meshPreparation_.ensureTileMesh(tile);
    }

    static TileLoadRequestOutcome requestMissingTilesWithBudget(
        Tileset& tileset,
        FrameResourceBudget& budget,
        const TileKey& firstKey,
        const TileKey& secondKey) {
        return tileset.requestMissingContent(
            {
                TileLoadRequest{
                    firstKey,
                    TileLoadPriorityGroup::Normal,
                    100.0},
                TileLoadRequest{
                    secondKey,
                    TileLoadPriorityGroup::Normal,
                    1.0},
            },
            &budget);
    }

    static void requestMissingTile(Tileset& tileset, const TileKey& key) {
        tileset.requestMissingContent({
            TileLoadRequest{
                key,
                TileLoadPriorityGroup::Normal}});
    }

    static std::string terrainCacheKey(Tileset&, const TileKey& key) {
        return TileCacheKey::forTile(key);
    }

    static void putTerrainCache(
        Tileset& tileset,
        const TileKey& key,
        std::unique_ptr<DecodedHeightmap> heightmap) {
        tileset.contentLifecycle_.heightmapTerrainCache()[
            terrainCacheKey(tileset, key)] =
            std::move(heightmap);
    }

    static bool containsPendingWorkFor(Tileset& tileset, const TileKey& key) {
        return tileset.contentLifecycle_
            .loadLifecycle()
            .containsWorkForCacheKey(terrainCacheKey(tileset, key));
    }

    static bool hasTerrainCache(Tileset& tileset, const TileKey& key) {
        return tileset.contentLifecycle_.heightmapTerrainCache().count(
                   terrainCacheKey(tileset, key)) > 0;
    }

    static bool claimContentUpload(Tileset& tileset, const TileKey& key) {
        FrameResourceBudgetConfig config;
        config.maxMainThreadFinalizesPerFrame = 1;
        FrameResourceBudget budget;
        budget.beginFrame(1, config);
        const std::string cacheKey = terrainCacheKey(tileset, key);
        std::lock_guard<std::mutex> lock(
            tileset.contentLifecycle_.loadLifecycle().mutex());
        tileset.contentLifecycle_.loadLifecycle().pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
                key,
                cacheKey,
                TileLoadPriorityGroup::Normal,
                0.0,
            TileLoadResult::fromContentResult(TileContentLoadResult::empty())});
        return tileset.contentLifecycle_
            .loadLifecycle()
            .pendingLoads()
            .takeHighestPriorityUpload(false, budget)
            .has_value();
    }

    static void queueLoad(
        Tileset& tileset,
        const TileKey& key,
        TileLoadPriorityGroup group) {
        tileset.loadQueue_.queue(
            key,
            group,
            std::numeric_limits<double>::max());
    }

    static void markEligibleForUnloading(
        Tileset& tileset,
        const TileKey& key) {
        tileset.cacheOwnership_.markEligibleForUnloading(
            terrainCacheKey(tileset, key));
    }

    static void updateTotalBytesUsed(Tileset& tileset) {
        tileset.cacheOwnership_.updateTotalBytesUsed();
    }

    static TilesetUpdateFrameRuntimeResult runUpdateFrameRuntime(
        Tileset& tileset,
        const FrameState& frameState,
        IPrepareRendererResources* pPrepRenderer = nullptr) {
        return TilesetUpdateFrameRuntime::run(
            tileset,
            frameState,
            pPrepRenderer);
    }

    static void queueContentUpload(
        Tileset& tileset,
        const TileKey& key,
        TileContentLoadResult&& result) {
        const std::string cacheKey = terrainCacheKey(tileset, key);
        std::lock_guard<std::mutex> lock(
            tileset.contentLifecycle_.loadLifecycle().mutex());
        tileset.contentLifecycle_.loadLifecycle().pendingLoads().addUpload(
            PendingTileLoad{
                TileLoadDomain::Content,
                key,
                cacheKey,
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadResult::fromContentResult(std::move(result))});
    }

    static void queueContentTerminalResult(
        Tileset& tileset,
        const TileKey& key,
        TileLoadStatus status) {
        const std::string cacheKey = terrainCacheKey(tileset, key);
        std::lock_guard<std::mutex> lock(
            tileset.contentLifecycle_.loadLifecycle().mutex());
        tileset.contentLifecycle_.loadLifecycle().pendingLoads()
            .addTerminalResult(PendingTileLoad{
                TileLoadDomain::Content,
                key,
                cacheKey,
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadResult::createTerminal(status)});
    }

    static void unloadCachedBytes(
        Tileset& tileset,
        int64_t maximumCachedBytes) {
        tileset.cacheOwnership_.unloadCachedBytes(maximumCachedBytes, nullptr);
    }

    static void unloadTileContent(Tileset& tileset, TilesetTile& tile) {
        tileset.cacheOwnership_.unloadTileContent(tile, nullptr);
    }

    static void requestMissingTilesWithPriorities(
        Tileset& tileset,
        const TileKey& firstKey,
        double firstPriority,
        const TileKey& secondKey,
        double secondPriority) {
        tileset.requestMissingContent({
            TileLoadRequest{
                firstKey,
                TileLoadPriorityGroup::Normal,
                firstPriority},
            TileLoadRequest{
                secondKey,
                TileLoadPriorityGroup::Normal,
                secondPriority}});
    }

    static TilesetTile* findTile(Tileset& tileset, const TileKey& key) {
        return tileset.tileRegistry_.findTile(key);
    }

    static void processPendingUploads(Tileset& tileset) {
        tileset.processPendingLoads(false, false, nullptr);
    }

    static bool isTileRenderable(Tileset& tileset, const TilesetTile& tile) {
        return TileSelectionRasterOverlayPreparer::isRenderable(
            tile,
            tileset.rasterOverlays_);
    }

    static void clearChildrenRecursively(Tileset& tileset, TilesetTile& tile) {
        tileset.cacheOwnership_.clearChildrenRecursively(&tile, nullptr);
    }

    static bool loadQueueContainsAny(
        const Tileset& tileset,
        const TileKey& key) {
        for (const TileLoadRequest& request : tileset.loadQueue_) {
            if (request.key == key) {
                return true;
            }
        }
        return false;
    }

    static void queueNormal(Tileset& tileset, const TileKey& key) {
        TileSelectionPlanAppender::queueTileLoad(
            tileset.loadQueue_,
            key,
            TileLoadPriorityGroup::Normal,
            std::numeric_limits<double>::max());
    }

    static void processPendingUploadsWithBudget(
        Tileset& tileset,
        FrameResourceBudget& budget) {
        tileset.processPendingLoads(false, false, nullptr, &budget);
    }

    static std::unique_ptr<Tileset> makeWithBothTerrainOwners(
        std::unique_ptr<TerrainProvider> legacyTerrainProvider,
        std::unique_ptr<TilesetContentProvider> contentProvider) {
        return std::unique_ptr<Tileset>(new Tileset(
            Tileset::ProviderOwnership{
                std::move(legacyTerrainProvider),
                std::move(contentProvider)},
            TileScheme::createGeographicTMS(),
            {},
            nullptr,
            TilesetOptions{}));
    }

    static bool hasLegacyTerrainProvider(const Tileset& tileset) {
        return tileset.legacyTerrainProvider_ != nullptr;
    }
};
} // namespace earth_engine

namespace {

template <typename T>
void appendPod(std::vector<uint8_t>& bytes, T value) {
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(T));
}

void writeBytes(const std::filesystem::path& path,
                const std::vector<uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

struct ExternalGltfBytes {
    std::vector<uint8_t> jsonBytes;
    std::vector<uint8_t> binBytes;
};

ExternalGltfBytes makeTriangleExternalGltfBytes() {
    std::vector<uint8_t> bin;
    const size_t positionsOffset = bin.size();
    appendPod<float>(bin, 0.0f);
    appendPod<float>(bin, 0.0f);
    appendPod<float>(bin, 0.0f);
    appendPod<float>(bin, 1.0f);
    appendPod<float>(bin, 0.0f);
    appendPod<float>(bin, 0.0f);
    appendPod<float>(bin, 0.0f);
    appendPod<float>(bin, 1.0f);
    appendPod<float>(bin, 0.0f);

    const size_t normalsOffset = bin.size();
    for (int i = 0; i < 3; ++i) {
        appendPod<float>(bin, 0.0f);
        appendPod<float>(bin, 0.0f);
        appendPod<float>(bin, 1.0f);
    }

    const size_t uvOffset = bin.size();
    appendPod<float>(bin, 0.0f);
    appendPod<float>(bin, 0.0f);
    appendPod<float>(bin, 1.0f);
    appendPod<float>(bin, 0.0f);
    appendPod<float>(bin, 0.0f);
    appendPod<float>(bin, 1.0f);

    const size_t indicesOffset = bin.size();
    appendPod<uint16_t>(bin, 0);
    appendPod<uint16_t>(bin, 1);
    appendPod<uint16_t>(bin, 2);
    while ((bin.size() % 4u) != 0u) {
        bin.push_back(0);
    }

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
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(positionsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(normalsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(uvOffset) + ",\"byteLength\":24}," +
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(indicesOffset) + ",\"byteLength\":6}" +
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

class BlockingHttpRequest final : public HttpRequest {
public:
    void cancel() override {}
};

class BlockingPlatformBridge final : public PlatformBridge {
public:
    void onMemoryPressure() override {}
    void onEnterBackground() override {}
    void onEnterForeground() override {}

    std::unique_ptr<HttpRequest> get(
        const std::string&,
        std::function<void(int, std::vector<uint8_t>)> callback,
        HttpRequestOptions = {}) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback_ = std::move(callback);
            entered_ = true;
        }
        cv_.notify_all();
        return std::make_unique<BlockingHttpRequest>();
    }

    int maximumActiveRequests() const override { return 11; }

    bool waitUntilEntered() {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(
            lock,
            std::chrono::seconds(2),
            [this]() { return entered_; });
    }

    bool complete(int statusCode, std::vector<uint8_t> body = {}) {
        std::function<void(int, std::vector<uint8_t>)> callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = std::move(callback_);
        }
        if (!callback) {
            return false;
        }
        callback(statusCode, std::move(body));
        return true;
    }

    std::string cacheDirectory() const override { return "/tmp"; }
    std::string documentsDirectory() const override { return "/tmp"; }
    std::unique_ptr<DecodedImage> decodeImage(
        const uint8_t*,
        size_t) override {
        return nullptr;
    }
    void log(LogLevel, const std::string&, const std::string&) override {}
    DeviceInfo deviceInfo() const override { return {}; }
    std::string getToken(const std::string&) const override { return {}; }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::function<void(int, std::vector<uint8_t>)> callback_;
    bool entered_ = false;
};

class ManualCompletionTerrainProvider final : public TerrainProvider {
public:
    struct PendingRequest {
        TileKey key;
        TerrainCallback callback;
    };

    std::string id() const override { return "manual-completion-terrain"; }
    std::string schemeId() const override { return "Geographic-TMS"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }

    bool supportsTile(const TileKey& key) const override {
        ++supportsTileCalls;
        return TerrainProvider::supportsTile(key);
    }

    std::string buildUrl(const TileKey&) const override {
        return "memory://manual-completion-terrain";
    }

    void requestTile(
        const TileKey& key,
        CancellationToken,
        TerrainCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        pendingRequests.push_back(PendingRequest{key, std::move(callback)});
    }

    ProviderRequestDiagnostics requestDiagnostics() const override {
        ProviderRequestDiagnostics diagnostics;
        diagnostics.maximumTransportActiveRequests =
            maximumTransportActiveRequests;
        return diagnostics;
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

        TerrainCallback callback = std::move(it->callback);
        pendingRequests.erase(it);
        callback(key, TerrainTileLoadResult::successWithHeightmap(std::move(heightmap)));
        return true;
    }

    std::unique_ptr<DecodedHeightmap> decodeTile(const uint8_t*, size_t)
        override {
        return nullptr;
    }

    std::vector<PendingRequest> pendingRequests;
    int maximumTransportActiveRequests = -1;
    mutable int supportsTileCalls = 0;
};

class ManualCompletionContentProvider final : public TilesetContentProvider {
public:
    struct PendingRequest {
        TileKey key;
        ContentCallback callback;
    };

    explicit ManualCompletionContentProvider(TileKey key)
        : keys_{std::move(key)} {}

    explicit ManualCompletionContentProvider(std::vector<TileKey> keys)
        : keys_(std::move(keys)) {}

    std::string id() const override { return "manual-completion-content"; }
    bool supportsTile(const TileKey& key) const override {
        return std::find(keys_.begin(), keys_.end(), key) != keys_.end();
    }
    std::vector<TileKey> rootTiles() const override { return keys_; }
    bool providesTerrainQuadtree() const override {
        return ownsTerrainQuadtree;
    }

    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
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

    bool completeWithEmpty(const TileKey& key) {
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
        callback(key, TileContentLoadResult::empty());
        return true;
    }

    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    ProviderRequestDiagnostics requestDiagnostics() const override {
        return diagnostics;
    }

    std::vector<PendingRequest> pendingRequests;
    ProviderRequestDiagnostics diagnostics;
    bool ownsTerrainQuadtree = false;

private:
    std::vector<TileKey> keys_;
};

class LifetimeTrackedTerrainProvider final : public TerrainProvider {
public:
    explicit LifetimeTrackedTerrainProvider(bool& destroyed)
        : destroyed_(destroyed) {}
    ~LifetimeTrackedTerrainProvider() override { destroyed_ = true; }

    std::string id() const override { return "lifetime-tracked-terrain"; }
    std::string schemeId() const override { return "Geographic-TMS"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://lifetime-tracked-terrain";
    }
    void requestTile(
        const TileKey&,
        CancellationToken,
        TerrainCallback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {}
    ProviderRequestDiagnostics requestDiagnostics() const override {
        ProviderRequestDiagnostics diagnostics;
        diagnostics.requestsStarted = 99;
        diagnostics.maximumTransportActiveRequests = 99;
        return diagnostics;
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(const uint8_t*, size_t)
        override {
        return nullptr;
    }

private:
    bool& destroyed_;
};

class SparseTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "sparse-terrain"; }
    std::string schemeId() const override { return "Geographic-TMS"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 4; }
    int tileSize() const override { return 2; }

    TileAvailabilityState availabilityState(const TileKey& key) const override {
        if (key.schemeId != schemeId()) {
            return TileAvailabilityState::NotAvailable;
        }
        if (key.z == 0) {
            return TileAvailabilityState::Available;
        }
        if (key.z == 1 && key.x == 0 && key.y == 0) {
            return TileAvailabilityState::Available;
        }
        return TileAvailabilityState::NotAvailable;
    }

    std::string buildUrl(const TileKey&) const override {
        return "memory://sparse-terrain";
    }

    void requestTile(
        const TileKey& key,
        CancellationToken,
        TerrainCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        ++requestCount;
        callback(key, TerrainTileLoadResult::retryLater());
    }

    std::unique_ptr<DecodedHeightmap> decodeTile(const uint8_t*, size_t)
        override {
        return nullptr;
    }

    int requestCount = 0;
};

class ContractTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "contract-terrain"; }
    std::string schemeId() const override { return "Geographic-TMS"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 2; }
    int tileSize() const override { return 2; }

    TileAvailabilityState availabilityState(const TileKey& key) const override {
        return key.schemeId == schemeId()
            ? TileAvailabilityState::Available
            : TileAvailabilityState::NotAvailable;
    }

    bool isAvailabilityBoundaryLevel(int level) const override {
        return level == 0;
    }

    std::string buildUrl(const TileKey&) const override {
        return "memory://contract-terrain";
    }

    void requestTile(
        const TileKey& key,
        CancellationToken,
        TerrainCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callback(key, TerrainTileLoadResult::retryLater());
    }

    std::unique_ptr<DecodedHeightmap> decodeTile(const uint8_t*, size_t)
        override {
        return nullptr;
    }

};

class DummyBuffer final : public Buffer {
public:
    explicit DummyBuffer(size_t byteSize) : byteSize_(byteSize) {}
    DummyBuffer(size_t byteSize, const void*) : byteSize_(byteSize) {}
    size_t size() const override { return byteSize_; }

private:
    size_t byteSize_ = 0;
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

class RecordingPrepareRendererResources final
    : public IPrepareRendererResources {
public:
    void attachRasterInMainThread(
        const TileKey& geometryKey,
        int32_t overlayIndex,
        std::shared_ptr<const RasterOverlayTile> rasterTile,
        Texture* texture,
        float,
        float,
        float,
        float) override {
        ++attachCount;
        lastGeometryKey = geometryKey;
        lastOverlayIndex = overlayIndex;
        lastRasterTile = std::move(rasterTile);
        lastTexture = texture;
    }

    void detachRasterInMainThread(
        const TileKey& geometryKey,
        int32_t overlayIndex) noexcept override {
        ++detachCount;
        lastDetachedGeometryKey = geometryKey;
        lastDetachedOverlayIndex = overlayIndex;
    }

    int attachCount = 0;
    int detachCount = 0;
    TileKey lastGeometryKey;
    TileKey lastDetachedGeometryKey;
    int32_t lastOverlayIndex = -1;
    int32_t lastDetachedOverlayIndex = -1;
    std::shared_ptr<const RasterOverlayTile> lastRasterTile;
    Texture* lastTexture = nullptr;
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
        return std::make_unique<DummyTexture>(desc.width, desc.height);
    }

    bool updateTextureRegion(
        Texture*,
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
    void submit(const RenderCommandList&) override {}
    void endFrame() override {}
    void onSurfaceCreated() override {}
    void onSurfaceChanged(int, int) override {}
    void onSurfaceDestroyed() override {}
};

std::unique_ptr<DecodedHeightmap> makeFlatHeightmap(float heightMeters) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights = {heightMeters, heightMeters, heightMeters, heightMeters};
    heightmap->minHeight = heightMeters;
    heightmap->maxHeight = heightMeters;
    return heightmap;
}

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
    primitive.indices = {0, 1, 2};
    model->primitives.push_back(std::move(primitive));
    return model;
}

SelectorView makeSelectorView(
    const Camera& camera,
    int viewportWidth,
    int viewportHeight) {
    SelectorView view;
    view.position = camera.position();
    view.direction = camera.direction();
    const double width = static_cast<double>(viewportWidth);
    const double height = static_cast<double>(viewportHeight);
    view.projectionMatrix = camera.projectionMatrix(width, height);
    view.frustum = Frustum::fromViewProjection(
        view.projectionMatrix * camera.viewMatrix());
    view.viewportHeightPixels = viewportHeight;
    return view;
}

} // namespace

TEST(TilesetRequestMissingBudgetTest,
     MainThreadUploadBudgetIsGlobalAcrossContentKinds) {
    TilesetOptions options;
    options.maximumSimultaneousTileLoads = 2;
    options.mainThreadLoadingTimeLimit = 1.0e-12;

    const TileKey lowPriorityContentKey{"Geographic-TMS", 1, 0, 0};
    const TileKey highPriorityContentKey{"Geographic-TMS", 1, 1, 0};

    auto contentProvider =
        std::make_unique<ManualCompletionContentProvider>(
            std::vector<TileKey>{
                lowPriorityContentKey,
                highPriorityContentKey});
    ManualCompletionContentProvider* rawContentProvider =
        contentProvider.get();
    DummyRenderDevice device;
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        &device,
        options,
        std::move(contentProvider));

    TilesetTestAccess::ensureTile(tileset, lowPriorityContentKey);
    TilesetTestAccess::ensureTile(tileset, highPriorityContentKey);
    TilesetTestAccess::requestMissingTilesWithPriorities(
        tileset,
        lowPriorityContentKey,
        100.0,
        highPriorityContentKey,
        1.0);

    ASSERT_EQ(rawContentProvider->pendingRequests.size(), 2u);
    EXPECT_TRUE(rawContentProvider->completeWithModel(
        lowPriorityContentKey,
        makeTriangleGltfModel()));
    EXPECT_TRUE(rawContentProvider->completeWithModel(
        highPriorityContentKey,
        makeTriangleGltfModel()));
    EXPECT_EQ(tileset.loadDiagnostics().pendingGltfTerrainUploads, 0);
    EXPECT_EQ(tileset.loadDiagnostics().pendingContentUploads, 2);

    TilesetTestAccess::processPendingUploads(tileset);

    TilesetTile* lowPriorityContentTile =
        TilesetTestAccess::findTile(tileset, lowPriorityContentKey);
    TilesetTile* highPriorityContentTile =
        TilesetTestAccess::findTile(tileset, highPriorityContentKey);
    ASSERT_NE(highPriorityContentTile, nullptr);
    EXPECT_EQ(
        highPriorityContentTile->content.loadState,
        TileLoadState::Done);
    EXPECT_EQ(
        highPriorityContentTile->content.contentKind,
        TileContentKind::Render);
    EXPECT_TRUE(
        highPriorityContentTile->content.renderContent.hasGltfModel());
    EXPECT_TRUE(highPriorityContentTile->content.renderContent
                    .hasGltfPrimitiveResources());

    const TilesetLoadDiagnostics diagnostics = tileset.loadDiagnostics();
    ASSERT_NE(lowPriorityContentTile, nullptr);
    EXPECT_EQ(
        lowPriorityContentTile->content.loadState,
        TileLoadState::ContentLoading);
    EXPECT_EQ(diagnostics.pendingGltfTerrainUploads, 0);
    EXPECT_EQ(diagnostics.pendingContentUploads, 1);
}

TEST(TilesetRequestMissingBudgetTest,
     ContentTerrainQuadtreeSuppressesLegacyTerrainRequestPath) {
    const TileKey key{"Geographic-TMS", 1, 0, 0};
    auto contentProvider =
        std::make_unique<ManualCompletionContentProvider>(key);
    contentProvider->ownsTerrainQuadtree = true;
    ManualCompletionContentProvider* rawContentProvider =
        contentProvider.get();

    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        {},
        std::move(contentProvider));
    TilesetTestAccess::ensureTile(tileset, key);

    TilesetTestAccess::requestMissingTile(tileset, key);

    ASSERT_EQ(rawContentProvider->pendingRequests.size(), 1u);
    EXPECT_EQ(rawContentProvider->pendingRequests.front().key, key);
    EXPECT_TRUE(rawContentProvider->completeWithEmpty(key));
}

TEST(
    TilesetRequestMissingBudgetTest,
    UpdateFrameUsesProviderTransportLaneForRasterBudget) {
    TilesetOptions options;
    options.maximumSimultaneousTileLoads = 20;

    auto provider = std::make_unique<ManualCompletionTerrainProvider>();
    provider->maximumTransportActiveRequests = 11;
    ManualCompletionTerrainProvider* rawProvider = provider.get();
    Tileset tileset = TilesetTestAccess::makeLegacyTerrainTileset(
        std::move(provider),
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        options);

    Camera camera;
    camera.lookAt(
        Vec3(Ellipsoid::WGS84().semiMajorAxis() * 2.0, 0.0, 0.0),
        Vec3(Ellipsoid::WGS84().semiMajorAxis(), 0.0, 0.0),
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 401;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));

    tileset.update(frameState);

    const TilesetLoadDiagnostics diagnostics = tileset.loadDiagnostics();
    EXPECT_EQ(diagnostics.resourceBudget.maxRasterNetworkRequestsPerFrame, 11u);
    EXPECT_EQ(diagnostics.resourceBudget.maxNetworkRequestsPerFrame, 20u);
    EXPECT_EQ(
        diagnostics.resourceBudget.maxTerrainContentNetworkRequestsPerFrame,
        20u);
    EXPECT_EQ(
        diagnostics.terrainProviderRequests.maximumTransportActiveRequests,
        11);

    while (!rawProvider->pendingRequests.empty()) {
        const TileKey pendingKey = rawProvider->pendingRequests.front().key;
        EXPECT_TRUE(rawProvider->completeWithHeightmap(
            pendingKey,
            makeFlatHeightmap(0.0f)));
    }
}

TEST(
    TilesetRequestMissingBudgetTest,
    UpdateFramePassesRendererPrepSinkToContentReplacementDetach) {
    const TileKey key{"Geographic-TMS", 0, 0, 0};
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        {});
    TilesetTile* tile = TilesetTestAccess::ensureTile(tileset, key);
    ASSERT_NE(nullptr, tile);

    auto existingModel = std::make_unique<GltfModel>();
    existingModel->rasterOverlayDetails.rasterOverlayProjections.push_back(
        RasterOverlayProjection::WebMercator);
    existingModel->rasterOverlayDetails.rasterOverlayRectangles.push_back(
        projectRectangleSimple(
            WebMercatorProjection(Ellipsoid::WGS84()),
            tile->bounds));
    tile->content.renderContent.prepareGltfContent(
        std::move(existingModel),
        Mat4::identity());
    tile->content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    tile->content.loadState = TileLoadState::Done;
    tile->content.contentKind = TileContentKind::Render;

    DebugImageryProvider imagery;
    auto rasterScheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider rasterProvider(imagery, *rasterScheme, nullptr);
    RasterMappedToTilesetTile& mapped =
        tile->rasterOverlayState.ensureMapping(0);
    std::vector<RasterOverlayProjection> missingProjections;
    ASSERT_EQ(
        RasterMappedToTilesetTile::MoreDetail::Unknown,
        mapped.update(
            key,
            tile->content.renderContent.rasterOverlayDetails(),
            512.0,
            512.0,
            rasterProvider,
            nullptr,
            missingProjections));
    ASSERT_NE(nullptr, mapped.getLoadingTile());
    mapped.getLoadingTile()->setTexture(std::make_unique<DummyTexture>(4, 4));

    RecordingPrepareRendererResources recorder;
    mapped.update(
        key,
        tile->content.renderContent.rasterOverlayDetails(),
        512.0,
        512.0,
        rasterProvider,
        &recorder,
        missingProjections);
    ASSERT_EQ(1, recorder.attachCount);
    ASSERT_EQ(0, recorder.detachCount);

    TilesetTestAccess::queueContentUpload(
        tileset,
        key,
        TileContentLoadResult::render(std::make_unique<GltfModel>()));

    Camera camera;
    camera.lookAt(
        Vec3(Ellipsoid::WGS84().semiMajorAxis() * 2.0, 0.0, 0.0),
        Vec3(Ellipsoid::WGS84().semiMajorAxis(), 0.0, 0.0),
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 402;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));

    TilesetTestAccess::runUpdateFrameRuntime(tileset, frameState, &recorder);

    EXPECT_EQ(1, recorder.detachCount);
    EXPECT_EQ(key, recorder.lastDetachedGeometryKey);
    EXPECT_EQ(0, recorder.lastDetachedOverlayIndex);
}

TEST(
    TilesetRequestMissingBudgetTest,
    UpdateFramePassesRendererPrepSinkToTerminalResultDetach) {
    const TileKey key{"Geographic-TMS", 0, 0, 0};
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        {});
    TilesetTile* tile = TilesetTestAccess::ensureTile(tileset, key);
    ASSERT_NE(nullptr, tile);

    auto existingModel = std::make_unique<GltfModel>();
    existingModel->rasterOverlayDetails.rasterOverlayProjections.push_back(
        RasterOverlayProjection::WebMercator);
    existingModel->rasterOverlayDetails.rasterOverlayRectangles.push_back(
        projectRectangleSimple(
            WebMercatorProjection(Ellipsoid::WGS84()),
            tile->bounds));
    tile->content.renderContent.prepareGltfContent(
        std::move(existingModel),
        Mat4::identity());
    tile->content.renderContent.addGltfPrimitiveResource(
        GltfPrimitiveRenderResources{});
    tile->content.loadState = TileLoadState::Done;
    tile->content.contentKind = TileContentKind::Render;

    DebugImageryProvider imagery;
    auto rasterScheme = TileScheme::createXYZWebMercator();
    RasterOverlayTileProvider rasterProvider(imagery, *rasterScheme, nullptr);
    RasterMappedToTilesetTile& mapped =
        tile->rasterOverlayState.ensureMapping(0);
    std::vector<RasterOverlayProjection> missingProjections;
    ASSERT_EQ(
        RasterMappedToTilesetTile::MoreDetail::Unknown,
        mapped.update(
            key,
            tile->content.renderContent.rasterOverlayDetails(),
            512.0,
            512.0,
            rasterProvider,
            nullptr,
            missingProjections));
    ASSERT_NE(nullptr, mapped.getLoadingTile());
    mapped.getLoadingTile()->setTexture(std::make_unique<DummyTexture>(4, 4));

    RecordingPrepareRendererResources recorder;
    mapped.update(
        key,
        tile->content.renderContent.rasterOverlayDetails(),
        512.0,
        512.0,
        rasterProvider,
        &recorder,
        missingProjections);
    ASSERT_EQ(1, recorder.attachCount);
    ASSERT_EQ(0, recorder.detachCount);

    TilesetTestAccess::queueContentTerminalResult(
        tileset,
        key,
        TileLoadStatus::Empty);

    Camera camera;
    camera.lookAt(
        Vec3(Ellipsoid::WGS84().semiMajorAxis() * 2.0, 0.0, 0.0),
        Vec3(Ellipsoid::WGS84().semiMajorAxis(), 0.0, 0.0),
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 403;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));

    TilesetTestAccess::runUpdateFrameRuntime(tileset, frameState, &recorder);

    EXPECT_EQ(1, recorder.detachCount);
    EXPECT_EQ(key, recorder.lastDetachedGeometryKey);
    EXPECT_EQ(0, recorder.lastDetachedOverlayIndex);
}

TEST(
    TilesetRequestMissingBudgetTest,
    ContentTerrainQuadtreeIgnoresLegacyTerrainProviderDiagnosticsForBudget) {
    const TileKey key{"Geographic-TMS", 0, 0, 0};
    TilesetOptions options;
    options.maximumSimultaneousTileLoads = 20;

    auto contentProvider =
        std::make_unique<ManualCompletionContentProvider>(key);
    contentProvider->ownsTerrainQuadtree = true;
    contentProvider->diagnostics.maximumTransportActiveRequests = 17;
    ManualCompletionContentProvider* rawContentProvider =
        contentProvider.get();

    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        options,
        std::move(contentProvider));

    Camera camera;
    camera.lookAt(
        Vec3(Ellipsoid::WGS84().semiMajorAxis() * 2.0, 0.0, 0.0),
        Vec3(Ellipsoid::WGS84().semiMajorAxis(), 0.0, 0.0),
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 402;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));

    tileset.update(frameState);

    const TilesetLoadDiagnostics diagnostics = tileset.loadDiagnostics();
    EXPECT_EQ(diagnostics.resourceBudget.maxRasterNetworkRequestsPerFrame, 17u);
    EXPECT_EQ(diagnostics.resourceBudget.maxNetworkRequestsPerFrame, 20u);
    EXPECT_EQ(
        diagnostics.terrainProviderRequests.maximumTransportActiveRequests,
        -1);
    EXPECT_EQ(
        diagnostics.contentProviderRequests.maximumTransportActiveRequests,
        17);
    EXPECT_TRUE(rawContentProvider->completeWithEmpty(key));
}

TEST(
    TilesetRequestMissingBudgetTest,
    ContentTerrainQuadtreeIgnoresHeightmapTerrainCacheForByteAccounting) {
    const TileKey key{"Geographic-TMS", 0, 0, 0};
    auto contentProvider =
        std::make_unique<ManualCompletionContentProvider>(key);
    contentProvider->ownsTerrainQuadtree = true;
    ManualCompletionContentProvider* rawContentProvider =
        contentProvider.get();

    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));
    TilesetTestAccess::ensureTile(tileset, key);

    auto heightmap = makeFlatHeightmap(42.0f);
    heightmap->metadataAvailability.resize(8);
    TilesetTestAccess::putTerrainCache(tileset, key, std::move(heightmap));

    TilesetTestAccess::updateTotalBytesUsed(tileset);

    EXPECT_EQ(tileset.totalBytesUsed(), 0);
    EXPECT_EQ(tileset.cachedTerrainTiles(), 0);

    Diagnostics sceneDiagnostics;
    SceneTilesetDiagnostics::reset(sceneDiagnostics);
    SceneTilesetDiagnostics::addTileset(sceneDiagnostics, tileset, true);
    EXPECT_EQ(sceneDiagnostics.surfaceMeshBytes, 0);
    EXPECT_EQ(sceneDiagnostics.terrainCachedTiles, 0);

    Camera camera;
    camera.lookAt(
        Vec3(Ellipsoid::WGS84().semiMajorAxis() * 2.0, 0.0, 0.0),
        Vec3(Ellipsoid::WGS84().semiMajorAxis(), 0.0, 0.0),
        Vec3::unitZ());

    FrameState frameState;
    frameState.frameId = 403;
    frameState.camera = &camera;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 800;
    frameState.selectorViews.push_back(makeSelectorView(camera, 800, 800));

    const TilesetUpdateFrameRuntimeResult updateResult =
        TilesetTestAccess::runUpdateFrameRuntime(tileset, frameState);
    EXPECT_EQ(updateResult.debugLog.terrainCacheSize, 0u);
    while (!rawContentProvider->pendingRequests.empty()) {
        const TileKey pendingKey =
            rawContentProvider->pendingRequests.front().key;
        EXPECT_TRUE(rawContentProvider->completeWithEmpty(pendingKey));
    }
}

TEST(
    TilesetRequestMissingBudgetTest,
    LoadDiagnosticsExposeContentProviderRequestDiagnostics) {
    const TileKey contentKey{"Geographic-TMS", 0, 0, 0};
    auto contentProvider =
        std::make_unique<ManualCompletionContentProvider>(contentKey);
    contentProvider->diagnostics.requestsStarted = 1;
    contentProvider->diagnostics.requestsCompleted = 0;
    contentProvider->diagnostics.activeWorkerBlockingRequests = 0;
    contentProvider->diagnostics.peakWorkerBlockingRequests = 0;
    contentProvider->diagnostics.maximumTransportActiveRequests = 11;
    ManualCompletionContentProvider* rawContentProvider =
        contentProvider.get();

    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    const TilesetLoadDiagnostics activeDiagnostics =
        tileset.loadDiagnostics();
    EXPECT_EQ(activeDiagnostics.contentProviderRequests.requestsStarted, 1);
    EXPECT_EQ(activeDiagnostics.contentProviderRequests.requestsCompleted, 0);
    EXPECT_EQ(
        activeDiagnostics
            .contentProviderRequests
            .activeWorkerBlockingRequests,
        0);
    EXPECT_EQ(
        activeDiagnostics
            .contentProviderRequests
            .peakWorkerBlockingRequests,
        0);
    EXPECT_EQ(
        activeDiagnostics
            .contentProviderRequests
            .maximumTransportActiveRequests,
        11);

    Diagnostics sceneDiagnostics;
    SceneTilesetDiagnostics::reset(sceneDiagnostics);
    SceneTilesetDiagnostics::addTileset(sceneDiagnostics, tileset, false);
    EXPECT_EQ(sceneDiagnostics.contentProviderRequestsStarted, 1);
    EXPECT_EQ(sceneDiagnostics.contentProviderRequestsCompleted, 0);
    EXPECT_EQ(sceneDiagnostics.contentProviderActiveWorkerBlockingRequests, 0);
    EXPECT_EQ(sceneDiagnostics.contentProviderPeakWorkerBlockingRequests, 0);
    EXPECT_EQ(sceneDiagnostics.contentTransportActiveRequestLimit, 11);

    rawContentProvider->diagnostics.requestsCompleted = 1;
    const TilesetLoadDiagnostics doneDiagnostics = tileset.loadDiagnostics();
    EXPECT_EQ(doneDiagnostics.contentProviderRequests.requestsCompleted, 1);
    EXPECT_EQ(
        doneDiagnostics
            .contentProviderRequests
            .activeWorkerBlockingRequests,
        0);
    EXPECT_EQ(
        doneDiagnostics
            .contentProviderRequests
            .peakWorkerBlockingRequests,
        0);
}

TEST(
    TilesetRequestMissingBudgetTest,
    ContentTerrainQuadtreeDropsLegacyTerrainOwnerAtConstruction) {
    const TileKey key{"Geographic-TMS", 0, 0, 0};
    bool legacyDestroyed = false;
    auto legacyTerrainProvider =
        std::make_unique<LifetimeTrackedTerrainProvider>(legacyDestroyed);
    auto contentProvider =
        std::make_unique<ManualCompletionContentProvider>(key);
    contentProvider->ownsTerrainQuadtree = true;
    contentProvider->diagnostics.requestsStarted = 7;
    contentProvider->diagnostics.maximumTransportActiveRequests = 7;

    std::unique_ptr<Tileset> tileset =
        TilesetTestAccess::makeWithBothTerrainOwners(
            std::move(legacyTerrainProvider),
            std::move(contentProvider));

    EXPECT_TRUE(legacyDestroyed);
    EXPECT_FALSE(TilesetTestAccess::hasLegacyTerrainProvider(*tileset));

    const TilesetLoadDiagnostics diagnostics = tileset->loadDiagnostics();
    EXPECT_EQ(diagnostics.terrainProviderRequests.requestsStarted, 0);
    EXPECT_EQ(
        diagnostics.terrainProviderRequests.maximumTransportActiveRequests,
        -1);
    EXPECT_EQ(diagnostics.contentProviderRequests.requestsStarted, 7);
    EXPECT_EQ(
        diagnostics.contentProviderRequests.maximumTransportActiveRequests,
        7);
}

TEST(
    TilesetRequestMissingBudgetTest,
    LoadDiagnosticsExposeAsyncContentProviderRequestDiagnostics) {
    BlockingPlatformBridge bridge;
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    auto contentProvider = std::make_unique<SingleGltfContentProvider>(
        rootKey,
        "https://example.invalid/content.glb",
        "diagnostic content fixture");
    contentProvider->setPlatformBridge(&bridge);

    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTestAccess::requestMissingTile(tileset, rootKey);
    ASSERT_TRUE(bridge.waitUntilEntered());

    const TilesetLoadDiagnostics activeDiagnostics =
        tileset.loadDiagnostics();
    EXPECT_EQ(activeDiagnostics.contentProviderRequests.requestsStarted, 1);
    EXPECT_EQ(activeDiagnostics.contentProviderRequests.requestsCompleted, 0);
    EXPECT_EQ(
        activeDiagnostics
            .contentProviderRequests
            .activeWorkerBlockingRequests,
        0);
    EXPECT_EQ(
        activeDiagnostics
            .contentProviderRequests
            .peakWorkerBlockingRequests,
        0);
    EXPECT_EQ(
        activeDiagnostics
            .contentProviderRequests
            .maximumTransportActiveRequests,
        11);

    Diagnostics sceneDiagnostics;
    SceneTilesetDiagnostics::reset(sceneDiagnostics);
    SceneTilesetDiagnostics::addTileset(sceneDiagnostics, tileset, false);
    EXPECT_EQ(sceneDiagnostics.contentProviderRequestsStarted, 1);
    EXPECT_EQ(sceneDiagnostics.contentProviderRequestsCompleted, 0);
    EXPECT_EQ(sceneDiagnostics.contentProviderActiveWorkerBlockingRequests, 0);
    EXPECT_EQ(sceneDiagnostics.contentProviderPeakWorkerBlockingRequests, 0);
    EXPECT_EQ(sceneDiagnostics.contentProviderExternalResourceRequestsStarted, 0);
    EXPECT_EQ(sceneDiagnostics.contentProviderExternalResourceRequestsCompleted, 0);
    EXPECT_EQ(
        sceneDiagnostics.contentProviderActiveExternalResourceBlockingRequests,
        0);
    EXPECT_EQ(
        sceneDiagnostics.contentProviderPeakExternalResourceBlockingRequests,
        0);
    EXPECT_EQ(sceneDiagnostics.contentTransportActiveRequestLimit, 11);

    ASSERT_TRUE(bridge.complete(404));
    for (int i = 0; i < 200 &&
                    tileset.loadDiagnostics()
                            .contentProviderRequests.requestsCompleted == 0;
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const TilesetLoadDiagnostics doneDiagnostics = tileset.loadDiagnostics();
    EXPECT_EQ(doneDiagnostics.contentProviderRequests.requestsCompleted, 1);
    EXPECT_EQ(
        doneDiagnostics
            .contentProviderRequests
            .activeWorkerBlockingRequests,
        0);
    EXPECT_EQ(
        doneDiagnostics
            .contentProviderRequests
            .peakWorkerBlockingRequests,
        0);
}

TEST(
    TilesetRequestMissingBudgetTest,
    SceneDiagnosticsExposeContentExternalResourceDiagnostics) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth-md-content-external-diagnostics";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "models");
    const ExternalGltfBytes externalGltf = makeTriangleExternalGltfBytes();
    writeBytes(root / "models" / "triangle.gltf", externalGltf.jsonBytes);
    writeBytes(root / "models" / "triangle.bin", externalGltf.binBytes);

    DummyRenderDevice device;
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    auto contentProvider = std::make_unique<SingleGltfContentProvider>(
        rootKey,
        "file://" + (root / "models" / "triangle.gltf").generic_string(),
        "external resource diagnostics fixture");

    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        &device,
        TilesetOptions{},
        std::move(contentProvider));

    TilesetTestAccess::requestMissingTile(tileset, rootKey);

    TilesetTile* tile = nullptr;
    for (int i = 0; i < 200; ++i) {
        TilesetTestAccess::processPendingUploads(tileset);
        tile = TilesetTestAccess::findTile(tileset, rootKey);
        const ProviderRequestDiagnostics& requests =
            tileset.loadDiagnostics().contentProviderRequests;
        if (tile &&
            tile->content.loadState == TileLoadState::Done &&
            tile->content.contentKind == TileContentKind::Render &&
            requests.externalResourceRequestsCompleted == 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const ProviderRequestDiagnostics& requests =
        tileset.loadDiagnostics().contentProviderRequests;
    ASSERT_NE(tile, nullptr);
    EXPECT_EQ(tile->content.loadState, TileLoadState::Done);
    EXPECT_EQ(tile->content.contentKind, TileContentKind::Render);
    EXPECT_EQ(requests.externalResourceRequestsStarted, 1);
    EXPECT_EQ(requests.externalResourceRequestsCompleted, 1);
    EXPECT_EQ(requests.activeExternalResourceBlockingRequests, 0);
    EXPECT_EQ(requests.peakExternalResourceBlockingRequests, 0);

    Diagnostics diagnostics;
    SceneTilesetDiagnostics::reset(diagnostics);
    SceneTilesetDiagnostics::addTileset(diagnostics, tileset, false);
    EXPECT_EQ(diagnostics.contentProviderExternalResourceRequestsStarted, 1);
    EXPECT_EQ(diagnostics.contentProviderExternalResourceRequestsCompleted, 1);
    EXPECT_EQ(
        diagnostics.contentProviderActiveExternalResourceBlockingRequests,
        0);
    EXPECT_EQ(
        diagnostics.contentProviderPeakExternalResourceBlockingRequests,
        0);
}

TEST(
    TilesetRequestMissingBudgetTest,
    LoadDiagnosticsExposeNativeLifecycleStates) {
    auto provider = std::make_unique<ManualCompletionTerrainProvider>();
    Tileset tileset = TilesetTestAccess::makeLegacyTerrainTileset(
        std::move(provider),
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{});

    const std::vector<TileKey> keys = {
        {"Geographic-TMS", 3, 0, 0},
        {"Geographic-TMS", 3, 1, 0},
        {"Geographic-TMS", 3, 2, 0},
        {"Geographic-TMS", 3, 3, 0},
        {"Geographic-TMS", 3, 4, 0},
        {"Geographic-TMS", 3, 5, 0},
        {"Geographic-TMS", 3, 6, 0}};

    std::vector<TilesetTile*> tiles;
    tiles.reserve(keys.size());
    for (const TileKey& key : keys) {
        tiles.push_back(TilesetTestAccess::ensureTile(tileset, key));
    }
    ASSERT_EQ(tiles.size(), keys.size());
    ASSERT_TRUE(std::all_of(
        tiles.begin(),
        tiles.end(),
        [](const TilesetTile* tile) { return tile != nullptr; }));

    const TilesetLoadDiagnostics baseline = tileset.loadDiagnostics();

    tiles[0]->content.loadState = TileLoadState::Unloading;
    tiles[0]->content.contentKind = TileContentKind::Unknown;
    tiles[1]->content.loadState = TileLoadState::FailedTemporarily;
    tiles[1]->content.contentKind = TileContentKind::Empty;
    tiles[2]->content.loadState = TileLoadState::Unloaded;
    tiles[2]->content.contentKind = TileContentKind::External;
    tiles[3]->content.loadState = TileLoadState::ContentLoading;
    tiles[3]->content.contentKind = TileContentKind::Render;
    tiles[4]->content.loadState = TileLoadState::ContentLoaded;
    tiles[4]->content.contentKind = TileContentKind::Unknown;
    tiles[5]->content.loadState = TileLoadState::Done;
    tiles[5]->content.contentKind = TileContentKind::Render;
    tiles[6]->content.loadState = TileLoadState::Failed;
    tiles[6]->content.contentKind = TileContentKind::Unknown;
    tiles[6]->rasterOverlayState.missingProjections().push_back(
        RasterOverlayProjection::Geographic);

    TilesetTestAccess::queueLoad(
        tileset,
        keys[0],
        TileLoadPriorityGroup::Preload);
    TilesetTestAccess::queueLoad(
        tileset,
        keys[1],
        TileLoadPriorityGroup::Normal);
    TilesetTestAccess::queueLoad(
        tileset,
        keys[2],
        TileLoadPriorityGroup::Urgent);
    TilesetTestAccess::markEligibleForUnloading(tileset, keys[5]);

    const TilesetLoadDiagnostics diagnostics = tileset.loadDiagnostics();
    EXPECT_EQ(diagnostics.loadQueuePreloadRequests, 1);
    EXPECT_EQ(diagnostics.loadQueueNormalRequests, 1);
    EXPECT_EQ(diagnostics.loadQueueUrgentRequests, 1);
    EXPECT_EQ(diagnostics.loadQueueTotal(), 3);
    EXPECT_EQ(
        diagnostics.loadUnloadingTiles,
        baseline.loadUnloadingTiles + 1);
    EXPECT_EQ(
        diagnostics.loadFailedTemporarilyTiles,
        baseline.loadFailedTemporarilyTiles + 1);
    EXPECT_EQ(
        diagnostics.loadUnloadedTiles,
        baseline.loadUnloadedTiles - 6);
    EXPECT_EQ(
        diagnostics.loadContentLoadingTiles,
        baseline.loadContentLoadingTiles + 1);
    EXPECT_EQ(
        diagnostics.loadContentLoadedTiles,
        baseline.loadContentLoadedTiles + 1);
    EXPECT_EQ(diagnostics.loadDoneTiles, baseline.loadDoneTiles + 1);
    EXPECT_EQ(diagnostics.loadFailedTiles, baseline.loadFailedTiles + 1);
    EXPECT_EQ(
        diagnostics.contentUnknownTiles,
        baseline.contentUnknownTiles - 4);
    EXPECT_EQ(diagnostics.contentEmptyTiles, baseline.contentEmptyTiles + 1);
    EXPECT_EQ(
        diagnostics.contentExternalTiles,
        baseline.contentExternalTiles + 1);
    EXPECT_EQ(diagnostics.contentRenderTiles, baseline.contentRenderTiles + 2);
    EXPECT_EQ(diagnostics.unloadQueueTiles, 1);
    EXPECT_EQ(
        diagnostics.missingRasterOverlayProjections,
        baseline.missingRasterOverlayProjections + 1);
}

TEST(
    TilesetRequestMissingBudgetTest,
    UpsampledChildQueuesParentUntilSourceReady) {
    auto provider = std::make_unique<SparseTerrainProvider>();
    SparseTerrainProvider* rawProvider = provider.get();
    Tileset tileset = TilesetTestAccess::makeLegacyTerrainTileset(
        std::move(provider),
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_EQ(root->children.size(), 4u);
    ASSERT_NE(root->children[1], nullptr);

    TilesetTile* upsampledChild = root->children[1];
    ASSERT_TRUE(upsampledChild->content.upsampledFromParent);

    TilesetTestAccess::requestMissingTile(tileset, upsampledChild->key);
    const TilesetLoadDiagnostics diagnostics = tileset.loadDiagnostics();

    EXPECT_EQ(upsampledChild->content.loadState, TileLoadState::Unloaded);
    EXPECT_EQ(diagnostics.pendingGltfTerrainUploads, 0);
    EXPECT_EQ(rawProvider->requestCount, 0);
    EXPECT_EQ(diagnostics.loadQueueUrgentRequests, 1);
    EXPECT_EQ(diagnostics.loadQueueNormalRequests, 0);
}

TEST(
    TilesetRequestMissingBudgetTest,
    TerrainAvailabilityBoundaryUsesProviderContract) {
    auto provider = std::make_unique<ContractTerrainProvider>();
    Tileset tileset = TilesetTestAccess::makeLegacyTerrainTileset(
        std::move(provider),
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    EXPECT_TRUE(root->children.empty());

    root->content.loadState = TileLoadState::Done;
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    EXPECT_EQ(root->children.size(), 4u);
}

TEST(
    TilesetRequestMissingBudgetTest,
    ClearChildrenErasesFlatMapDescendants) {
    auto provider = std::make_unique<SparseTerrainProvider>();
    Tileset tileset = TilesetTestAccess::makeLegacyTerrainTileset(
        std::move(provider),
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);

    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(1.0f));
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_EQ(root->children.size(), 4u);
    ASSERT_NE(root->children[1], nullptr);

    const TileKey childKey = root->children[1]->key;
    TilesetTestAccess::clearChildrenRecursively(tileset, *root);

    EXPECT_TRUE(root->children.empty());
    EXPECT_EQ(TilesetTestAccess::findTile(tileset, childKey), nullptr);

    TilesetTile* recreated = TilesetTestAccess::ensureTile(tileset, childKey);
    ASSERT_NE(recreated, nullptr);
    EXPECT_EQ(recreated->parent, root);
    ASSERT_EQ(root->children.size(), 1u);
    EXPECT_EQ(root->children.front(), recreated);
}

TEST(
    TilesetRequestMissingBudgetTest,
    ClearChildrenErasesClaimedUploadDescendantWork) {
    auto provider = std::make_unique<SparseTerrainProvider>();
    Tileset tileset = TilesetTestAccess::makeLegacyTerrainTileset(
        std::move(provider),
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);

    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(1.0f));
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_FALSE(root->children.empty());
    ASSERT_NE(root->children.front(), nullptr);

    const TileKey childKey = root->children.front()->key;
    TilesetTestAccess::queueNormal(tileset, childKey);
    ASSERT_TRUE(TilesetTestAccess::claimContentUpload(tileset, childKey));
    ASSERT_TRUE(TilesetTestAccess::containsPendingWorkFor(tileset, childKey));

    TilesetTestAccess::clearChildrenRecursively(tileset, *root);

    EXPECT_TRUE(root->children.empty());
    EXPECT_EQ(TilesetTestAccess::findTile(tileset, childKey), nullptr);
    EXPECT_FALSE(TilesetTestAccess::loadQueueContainsAny(tileset, childKey));
    EXPECT_FALSE(TilesetTestAccess::containsPendingWorkFor(tileset, childKey));
}

TEST(
    TilesetRequestMissingBudgetTest,
    ClearChildrenIgnoresStaleContentCallback) {
    const TileKey contentKey{"Geographic-TMS", 1, 0, 0};
    auto provider =
        std::make_unique<ManualCompletionContentProvider>(contentKey);
    ManualCompletionContentProvider* rawProvider = provider.get();
    DummyRenderDevice device;
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        &device,
        TilesetOptions{},
        std::move(provider));

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    TilesetTile* child = TilesetTestAccess::ensureTile(tileset, contentKey);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(child, nullptr);
    ASSERT_EQ(child->parent, root);

    TilesetTestAccess::requestMissingTile(tileset, contentKey);
    ASSERT_EQ(rawProvider->pendingRequests.size(), 1u);

    TilesetTestAccess::clearChildrenRecursively(tileset, *root);
    ASSERT_EQ(TilesetTestAccess::findTile(tileset, contentKey), nullptr);

    EXPECT_TRUE(rawProvider->completeWithModel(
        contentKey,
        makeTriangleGltfModel()));
    TilesetTestAccess::processPendingUploads(tileset);

    EXPECT_EQ(TilesetTestAccess::findTile(tileset, contentKey), nullptr);
    const TilesetLoadDiagnostics diagnostics = tileset.loadDiagnostics();
    EXPECT_EQ(diagnostics.pendingTerrainTotal(), 0);
    EXPECT_EQ(diagnostics.pendingContentTotal(), 0);
}

TEST(
    TilesetRequestMissingBudgetTest,
    UnloadKeepsParentWithReferencedDescendant) {
    auto provider = std::make_unique<SparseTerrainProvider>();
    Tileset tileset = TilesetTestAccess::makeLegacyTerrainTileset(
        std::move(provider),
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);

    auto heightmap = makeFlatHeightmap(1.0f);
    heightmap->metadataAvailability.resize(1);
    TilesetTestAccess::putTerrainCache(tileset, rootKey, std::move(heightmap));
    root->content.contentKind = TileContentKind::Render;
    root->content.loadState = TileLoadState::Done;
    root->content.renderContent.setMeshReady(true);
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_FALSE(root->children.empty());
    ASSERT_NE(root->children.front(), nullptr);

    const TileKey childKey = root->children.front()->key;
    root->children.front()->addReference();
    TilesetTestAccess::markEligibleForUnloading(tileset, rootKey);
    TilesetTestAccess::updateTotalBytesUsed(tileset);
    TilesetTestAccess::unloadCachedBytes(tileset, 0);

    EXPECT_EQ(TilesetTestAccess::findTile(tileset, rootKey), root);
    EXPECT_NE(TilesetTestAccess::findTile(tileset, childKey), nullptr);
}

TEST(
    TilesetRequestMissingBudgetTest,
    UnloadQueueIgnoresUnloadedUnknownTiles) {
    auto provider = std::make_unique<SparseTerrainProvider>();
    Tileset tileset = TilesetTestAccess::makeLegacyTerrainTileset(
        std::move(provider),
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);

    TilesetTestAccess::markEligibleForUnloading(tileset, rootKey);
    EXPECT_EQ(tileset.loadDiagnostics().unloadQueueTiles, 0);

    root->content.contentKind = TileContentKind::Empty;
    root->content.loadState = TileLoadState::Done;
    TilesetTestAccess::markEligibleForUnloading(tileset, rootKey);
    EXPECT_EQ(tileset.loadDiagnostics().unloadQueueTiles, 1);
}

TEST(
    TilesetRequestMissingBudgetTest,
    UnloadRenderContentPreservesLoadedChildren) {
    auto provider = std::make_unique<SparseTerrainProvider>();
    Tileset tileset = TilesetTestAccess::makeLegacyTerrainTileset(
        std::move(provider),
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);

    auto rootHeightmap = makeFlatHeightmap(1.0f);
    rootHeightmap->metadataAvailability.resize(1);
    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        std::move(rootHeightmap));
    root->content.contentKind = TileContentKind::Render;
    root->content.loadState = TileLoadState::Done;
    root->content.renderContent.setMeshReady(true);

    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_FALSE(root->children.empty());
    ASSERT_NE(root->children.front(), nullptr);

    TilesetTile* child = root->children.front();
    const TileKey childKey = child->key;
    auto childHeightmap = makeFlatHeightmap(2.0f);
    childHeightmap->metadataAvailability.resize(1);
    TilesetTestAccess::putTerrainCache(
        tileset,
        childKey,
        std::move(childHeightmap));
    child->content.contentKind = TileContentKind::Render;
    child->content.loadState = TileLoadState::Done;
    child->content.renderContent.setMeshReady(true);

    TilesetTestAccess::markEligibleForUnloading(tileset, rootKey);
    TilesetTestAccess::updateTotalBytesUsed(tileset);
    TilesetTestAccess::unloadCachedBytes(tileset, 0);

    TilesetTile* rootAfter = TilesetTestAccess::findTile(tileset, rootKey);
    TilesetTile* childAfter = TilesetTestAccess::findTile(tileset, childKey);
    ASSERT_EQ(rootAfter, root);
    EXPECT_EQ(rootAfter->content.loadState, TileLoadState::Unloaded);
    EXPECT_EQ(rootAfter->content.contentKind, TileContentKind::Unknown);
    EXPECT_FALSE(rootAfter->content.renderContent.isMeshReady());
    ASSERT_EQ(childAfter, child);
    EXPECT_EQ(childAfter->content.loadState, TileLoadState::Done);
    EXPECT_EQ(childAfter->content.contentKind, TileContentKind::Render);
    EXPECT_EQ(childAfter->parent, rootAfter);
}

TEST(
    TilesetRequestMissingBudgetTest,
    UnloadExternalContentClearsChildren) {
    auto provider = std::make_unique<SparseTerrainProvider>();
    Tileset tileset = TilesetTestAccess::makeLegacyTerrainTileset(
        std::move(provider),
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);

    root->content.contentKind = TileContentKind::External;
    root->content.loadState = TileLoadState::Done;
    root->unconditionallyRefine = true;
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_FALSE(root->children.empty());
    ASSERT_NE(root->children.front(), nullptr);

    const TileKey childKey = root->children.front()->key;
    auto childHeightmap = makeFlatHeightmap(3.0f);
    childHeightmap->metadataAvailability.resize(1);
    TilesetTestAccess::putTerrainCache(
        tileset,
        childKey,
        std::move(childHeightmap));

    TilesetTestAccess::markEligibleForUnloading(tileset, rootKey);
    TilesetTestAccess::updateTotalBytesUsed(tileset);
    TilesetTestAccess::unloadCachedBytes(tileset, 0);

    TilesetTile* rootAfter = TilesetTestAccess::findTile(tileset, rootKey);
    ASSERT_EQ(rootAfter, root);
    EXPECT_TRUE(rootAfter->children.empty());
    EXPECT_EQ(rootAfter->content.loadState, TileLoadState::Unloaded);
    EXPECT_EQ(rootAfter->content.contentKind, TileContentKind::Unknown);
    EXPECT_EQ(TilesetTestAccess::findTile(tileset, childKey), nullptr);
}

TEST(
    TilesetRequestMissingBudgetTest,
    DirectExternalContentUnloadClearsChildrenAfterReferencesRelease) {
    auto provider = std::make_unique<SparseTerrainProvider>();
    Tileset tileset = TilesetTestAccess::makeLegacyTerrainTileset(
        std::move(provider),
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(root, nullptr);

    root->content.contentKind = TileContentKind::External;
    root->content.loadState = TileLoadState::Done;
    root->unconditionallyRefine = true;
    TilesetTestAccess::ensureTileChildren(tileset, *root);
    ASSERT_FALSE(root->children.empty());
    ASSERT_NE(root->children.front(), nullptr);

    const TileKey childKey = root->children.front()->key;
    auto childHeightmap = makeFlatHeightmap(5.0f);
    childHeightmap->metadataAvailability.resize(1);
    TilesetTestAccess::putTerrainCache(
        tileset,
        childKey,
        std::move(childHeightmap));

    root->addReference();
    TilesetTestAccess::unloadTileContent(tileset, *root);
    EXPECT_EQ(root->content.loadState, TileLoadState::Done);
    EXPECT_EQ(root->content.contentKind, TileContentKind::External);
    EXPECT_FALSE(root->children.empty());
    EXPECT_NE(TilesetTestAccess::findTile(tileset, childKey), nullptr);
    root->clearReferences();

    TilesetTestAccess::unloadTileContent(tileset, *root);

    TilesetTile* rootAfter = TilesetTestAccess::findTile(tileset, rootKey);
    ASSERT_EQ(rootAfter, root);
    EXPECT_TRUE(rootAfter->children.empty());
    EXPECT_EQ(rootAfter->content.loadState, TileLoadState::Unloaded);
    EXPECT_EQ(rootAfter->content.contentKind, TileContentKind::Unknown);
    EXPECT_EQ(TilesetTestAccess::findTile(tileset, childKey), nullptr);
}
