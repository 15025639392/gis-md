#pragma once

#include "GltfModel.h"
#include "../core/math/Mat4.h"
#include "../platform/bridge/PlatformBridge.h"
#include "../threading/CancellationToken.h"
#include "../tiling/TileKey.h"
#include "../tiling/TileBoundingVolume.h"
#include "../tiling/TileRefine.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

enum class TileContentLoadStatus {
    Render,
    Empty,
    External,
    RetryLater,
    Failed,
    Cancelled
};

struct TilesetContentTileMetadata {
    TileKey key;
    std::optional<TileKey> parentKey;
    std::vector<TileKey> childKeys;
    Rectangle bounds;
    bool hasExplicitBounds = false;
    std::optional<TileBoundingVolume> boundingVolume;
    std::optional<TileBoundingVolume> viewerRequestVolume;
    std::optional<TileBoundingVolume> contentBoundingVolume;
    Mat4 transform = Mat4::identity();
    double geometricError = 0.0;
    TileRefine refine = TileRefine::Replace;
    bool unconditionallyRefine = false;
};

struct TileContentLoadResult {
    TileContentLoadStatus status = TileContentLoadStatus::Failed;
    std::unique_ptr<GltfModel> gltfModel;
    Mat4 contentTransform = Mat4::identity();

    static TileContentLoadResult render(std::unique_ptr<GltfModel> model) {
        TileContentLoadResult result;
        result.status = model ? TileContentLoadStatus::Render
                              : TileContentLoadStatus::Failed;
        result.gltfModel = std::move(model);
        return result;
    }

    static TileContentLoadResult empty() {
        TileContentLoadResult result;
        result.status = TileContentLoadStatus::Empty;
        return result;
    }

    static TileContentLoadResult retryLater() {
        TileContentLoadResult result;
        result.status = TileContentLoadStatus::RetryLater;
        return result;
    }

    static TileContentLoadResult failed() {
        TileContentLoadResult result;
        result.status = TileContentLoadStatus::Failed;
        return result;
    }

    static TileContentLoadResult cancelled() {
        TileContentLoadResult result;
        result.status = TileContentLoadStatus::Cancelled;
        return result;
    }

    static TileContentLoadResult external() {
        TileContentLoadResult result;
        result.status = TileContentLoadStatus::External;
        return result;
    }
};

class TilesetContentProvider {
public:
    virtual ~TilesetContentProvider() = default;

    virtual std::string id() const = 0;
    virtual bool supportsTile(const TileKey& key) const = 0;
    virtual std::vector<TileKey> rootTiles() const { return {}; }
    virtual std::optional<TilesetContentTileMetadata> tileMetadata(
        const TileKey&) const {
        return std::nullopt;
    }
    virtual std::vector<TileKey> childTiles(const TileKey&) const {
        return {};
    }

    using ContentCallback = std::function<void(
        const TileKey&, TileContentLoadResult)>;

    virtual void requestTileContent(const TileKey& key,
                                    CancellationToken token,
                                    ContentCallback callback) = 0;

    virtual TileContentLoadResult decodeContent(
        const uint8_t* data,
        size_t size) = 0;
};

/// A real glTF/GLB/B3DM content provider for a single tile. This is the current
/// equivalent of a minimal cesium-native TilesetContentLoader: it produces
/// TileRenderContent data, not terrain meshes.
class SingleGltfContentProvider final : public TilesetContentProvider {
public:
    SingleGltfContentProvider(TileKey contentKey,
                              std::string url,
                              std::string name = "glTF content");
    SingleGltfContentProvider(TileKey contentKey,
                              std::vector<uint8_t> bytes,
                              std::string name = "glTF content");

    std::string id() const override;
    bool supportsTile(const TileKey& key) const override;
    void requestTileContent(const TileKey& key,
                            CancellationToken token,
                            ContentCallback callback) override;
    TileContentLoadResult decodeContent(const uint8_t* data,
                                        size_t size) override;

    const TileKey& contentKey() const { return contentKey_; }
    const std::string& url() const { return url_; }
    void setPlatformBridge(PlatformBridge* bridge) { platformBridge_ = bridge; }
    const Mat4& contentTransform() const { return contentTransform_; }
    void setContentTransform(const Mat4& transform) {
        contentTransform_ = transform;
    }
    void setEastNorthUpPlacementDegrees(double longitudeDegrees,
                                        double latitudeDegrees,
                                        double heightMeters,
                                        double uniformScale = 1.0);

private:
    std::vector<uint8_t> httpGet(const std::string& url) const;

    TileKey contentKey_;
    std::string url_;
    std::string name_;
    std::vector<uint8_t> bytes_;
    Mat4 contentTransform_ = Mat4::identity();
    PlatformBridge* platformBridge_ = nullptr;
};

/// 3D Tiles tileset.json content provider aligned with cesium-native
/// TilesetJsonLoader for explicit tile trees. It parses JSON tile transforms,
/// refine modes, geometric errors, bounding volumes, content URI/url, and
/// external tileset JSON content.
class TilesetJsonContentProvider final : public TilesetContentProvider {
public:
    TilesetJsonContentProvider(std::string tilesetJsonUrl,
                               std::vector<uint8_t> tilesetJsonBytes,
                               std::string name = "3D Tiles");

    std::string id() const override;
    bool supportsTile(const TileKey& key) const override;
    std::vector<TileKey> rootTiles() const override;
    std::optional<TilesetContentTileMetadata> tileMetadata(
        const TileKey& key) const override;
    std::vector<TileKey> childTiles(const TileKey& key) const override;
    void requestTileContent(const TileKey& key,
                            CancellationToken token,
                            ContentCallback callback) override;
    TileContentLoadResult decodeContent(const uint8_t* data,
                                        size_t size) override;

    bool valid() const;
    void setPlatformBridge(PlatformBridge* bridge) { platformBridge_ = bridge; }

private:
    enum class TileRecordContentKind {
        Empty,
        External,
        Uri,
        Unsupported
    };

    struct TileRecord {
        TilesetContentTileMetadata metadata;
        TileRecordContentKind contentKind = TileRecordContentKind::Empty;
        Mat4 gltfUpAxisTransform = Mat4::identity();
        std::string contentUri;
        std::string resolvedContentUrl;
    };

    bool parseTilesetJson(const uint8_t* data,
                          size_t size,
                          const std::string& baseUrl,
                          const Mat4& parentTransform,
                          TileRefine parentRefine,
                          double parentGeometricError,
                          const std::optional<TileKey>& externalParentKey,
                          bool wrapRoot);
    TileContentLoadResult decodeRenderableContent(const uint8_t* data,
                                                  size_t size,
                                                  const Mat4& transform,
                                                  const Mat4& gltfUpAxisTransform,
                                                  const std::string& contentUrl);
    std::vector<uint8_t> httpGet(const std::string& url) const;

    std::string tilesetJsonUrl_;
    std::string name_;
    std::string schemeId_;
    PlatformBridge* platformBridge_ = nullptr;
    mutable std::mutex mutex_;
    std::vector<TileKey> rootKeys_;
    std::unordered_map<std::string, TileRecord> records_;
    int nextTileOrdinal_ = 0;
    bool valid_ = false;
};

} // namespace earth_engine
