#pragma once

#include "TileKey.h"
#include "SurfaceTile.h"
#include "../core/math/Rectangle.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace earth_engine {

class RasterOverlayTile;
class RasterOverlayTileProvider;
class FrameResourceBudget;
class IPrepareRendererResources;
class Texture;
struct TileBoundingVolume;
struct TilesetTile;

/// cesium-native RasterMappedTo3DTile equivalent.
/// Links a geometry tile to an imagery tile with state management.
///
/// Holds RasterOverlayTile shared handles for the desired loading tile and the
/// currently renderable ready tile. Raw pointers returned by accessors are
/// non-owning views; the mapping keeps tile/texture lifetime stable through
/// the shared handles.
///
/// State machine:
///   Unattached → (loading starts) → TemporarilyAttached (parent fallback)
///   → (loading completes) → Attached (own texture ready)
///
/// Key invariants:
///   - _pLoadingTile : the tile we WANT (may be Loading/Failed) — may be nullptr
///   - _pReadyTile   : the tile we RENDER (always Loaded/Done) — may be nullptr
///   - When _pLoadingTile == nullptr && _pReadyTile != nullptr, state is Attached
///   - When both non-null, state is TemporarilyAttached
///   - When both nullptr, state is Unattached
///   - _originalFailed is set once on original tile failure, never cleared
class RasterMappedToTilesetTile {
public:
    /// Aligned with cesium-native AttachmentState values
    enum class State {
        Unattached = 0,
        TemporarilyAttached = 1,
        Attached = 2
    };

    /// Aligned with cesium-native RasterOverlayTile::MoreDetailAvailable
    enum class MoreDetail {
        No = 0,
        Yes = 1,
        Unknown = 2
    };

    enum class ReadyTileSource {
        None = 0,
        Real = 1,
        Ancestor = 2
    };

    struct SourceTileList {
        int sourceZoom = 0;
        Rectangle sourceBounds = Rectangle::MAXIMUM;
        std::vector<TileKey> sourceKeys;
        int minX = 0;
        int minY = 0;
        int maxX = 0;
        int maxY = 0;

        bool empty() const { return sourceKeys.empty(); }
    };

    RasterMappedToTilesetTile();
    ~RasterMappedToTilesetTile();

    RasterMappedToTilesetTile(const RasterMappedToTilesetTile&) = delete;
    RasterMappedToTilesetTile& operator=(const RasterMappedToTilesetTile&) = delete;

    /// cesium-native RasterMappedTo3DTile::update — 7-step flow.
    /// @param geometryKey   The geometry tile's quadtree key.
    /// @param overlayDetails The render content's raster overlay details.
    /// @param tileProvider   The imagery tile provider (for tile creation/lookup).
    /// @param pPrepRenderer Resource lifecycle notification sink.
    /// @param parentTile     Parent geometry tile (for step 2 failure fallback).
    /// @param overlayIndex   Our index in parent's rasterOverlays vector.
    /// @return MoreDetailAvailable status.
    MoreDetail update(const TileKey& geometryKey,
                      const RasterOverlayDetails& overlayDetails,
                      double targetScreenPixelsX,
                      double targetScreenPixelsY,
                      RasterOverlayTileProvider& tileProvider,
                      IPrepareRendererResources* pPrepRenderer,
                      std::vector<RasterOverlayProjection>& missingProjections,
                      const TilesetTile* parentTile = nullptr,
                      size_t overlayIndex = 0,
                      bool hasRenderContentDetails = true,
                      std::optional<Rectangle> boundingVolumeRectangle =
                          std::nullopt);

    /// cesium-native: check if a higher-resolution tile could be loaded.
    bool isMoreDetailAvailable() const;

    /// True while a real raster tile is still loading or waiting for
    /// failure/loaded-state consumption. Ready ancestor fallbacks should not
    /// let traversal skip this lifecycle update.
    bool hasPendingNonPlaceholderLoadingTile() const;

    /// True when a subsequent update can only repeat the Attached fast path.
    bool hasStableUpdateState() const;

    /// 消费一个已经加载完的 loading 瓦片,把它提升为 ready —— 等价于 update()
    /// 的 Step 3,但**不做任何几何解算**。
    ///
    /// 为什么需要它:调用 update() 的三条路径(渲染项循环、
    /// TileSelectionRasterOverlayPreparer、可见瓦片循环的 Done 分支)全都要求
    /// 几何瓦片 Done+Render。停在 Failed 态、只有 fill 代理的瓦片一条都不满足,
    /// 而可见瓦片循环对"已有 mapping 的非 Done 瓦片"只泵 advanceThrottledLoads
    /// ——「只发不收」。结果是影像下载完了却永远躺在 loading 槽里,瓦片一直不
    /// 可画 → 每帧被 finalizer 丢弃 → 破洞(实测 z4/z5,见 HoleNoTex 探针)。
    ///
    /// @return true 表示这次真的完成了一次提升。
    bool promoteLoadedTileWithoutGeometryWork(
        IPrepareRendererResources* pPrepRenderer);

    /// Keep the stable ready tile resident without re-running update().
    void markStableReadyTileUsed();

    /// cesium-native: detach this raster from the geometry tile.
    void detachFromTile(IPrepareRendererResources* pPrepRenderer);

    /// Drop provider tile handles after the geometry tile stops rendering.
    /// This lets the provider evict unreferenced raster tiles safely.
    void releaseTileReferences(IPrepareRendererResources* pPrepRenderer);

    /// Materialize only the renderer attachment for a ready raster tile.
    /// Used when selection already advanced the mapping state in this frame.
    void attachReadyTileInMainThread(
        IPrepareRendererResources* pPrepRenderer);

    /// cesium-native: throttled load via the Provider.
    bool loadThrottled(RasterOverlayTileProvider& tileProvider,
                       FrameResourceBudget* budget = nullptr);

    /// Compute UV translation/scale from geometry↔imagery rectangles.
    void computeTranslationAndScale(const Rectangle& geometryBounds,
                                    const Rectangle& imageryBounds,
                                    bool invertedVCoordinate = false);

    // ── 目标几何缓存(闸3:cesium 里几何矩形 + computeDesiredScreenPixels 是
    // addTileOverlays 的「加载时一次」动作,不随帧/相机变;updateTileOverlays
    // 每帧只做轻量 update()。这里把三角开销缓存到映射槽,消除 Done 瓦片每帧
    // 重算)。键=(overlayDetails 指针, projection)。内容重载→details 指针变→
    // 失效重算。仅当 details 非空(真渲染内容)才缓存;bounds 回退路径(罕见、
    // 上采样 bounds 可能变)不缓存,照旧每帧算,零 staleness 风险。──
    bool targetGeometryCacheValid(const void* detailsPtr,
                                  RasterOverlayProjection projection) const {
        return targetGeometryCached_ &&
               cachedDetailsPtr_ == detailsPtr &&
               cachedProjection_ == projection;
    }
    void cacheTargetGeometry(
        const void* detailsPtr,
        RasterOverlayProjection projection,
        double screenPixelsX,
        double screenPixelsY,
        const std::optional<Rectangle>& boundingVolumeRectangle) {
        targetGeometryCached_ = true;
        cachedDetailsPtr_ = detailsPtr;
        cachedProjection_ = projection;
        cachedTargetScreenPixelsX_ = screenPixelsX;
        cachedTargetScreenPixelsY_ = screenPixelsY;
        cachedBoundingVolumeRectangle_ = boundingVolumeRectangle;
    }
    double cachedTargetScreenPixelsX() const {
        return cachedTargetScreenPixelsX_;
    }
    double cachedTargetScreenPixelsY() const {
        return cachedTargetScreenPixelsY_;
    }
    const std::optional<Rectangle>& cachedBoundingVolumeRectangle() const {
        return cachedBoundingVolumeRectangle_;
    }

    // ── Accessors (aligned with cesium-native naming) ──

    /// The loading tile (may be nullptr). Aligned with getLoadingTile().
    const RasterOverlayTile* getLoadingTile() const { return _pLoadingTile.get(); }
    RasterOverlayTile* getLoadingTile() { return _pLoadingTile.get(); }
    const std::shared_ptr<RasterOverlayTile>& getLoadingTileHandle() const {
        return _pLoadingTile;
    }

    /// The ready tile (may be nullptr). Aligned with getReadyTile().
    const RasterOverlayTile* getReadyTile() const { return _pReadyTile.get(); }
    RasterOverlayTile* getReadyTile() { return _pReadyTile.get(); }
    const std::shared_ptr<RasterOverlayTile>& getReadyTileHandle() const {
        return _pReadyTile;
    }
    ReadyTileSource getReadyTileSource() const { return readyTileSource_; }
    const SourceTileList& getMappedSourceTiles() const {
        return mappedSourceTiles_;
    }
    bool usesDirectRasterTile() const { return directRasterTile_; }

    /// Aligned with getTextureCoordinateID().
    /// Returns the raster-overlay texture coordinate index for this projection.
    int32_t getTextureCoordinateID() const { return textureCoordinateID_; }

    /// Aligned with getState().
    State getState() const { return state_; }

    /// Aligned with getTranslation() / getScale().
    float getTranslationU() const { return offsetU_; }
    float getTranslationV() const { return offsetV_; }
    float getScaleU() const { return scaleU_; }
    float getScaleV() const { return scaleV_; }

    // Retained renderer/cache accessors.
    State state() const { return state_; }
    Texture* texture() const;
    float offsetU() const { return offsetU_; }
    float offsetV() const { return offsetV_; }
    float scaleU() const { return scaleU_; }
    float scaleV() const { return scaleV_; }

    uint64_t runtimeStateSignature() const;

private:
    void clearTileOwnershipState();

    State state_ = State::Unattached;

    /// The tile we WANT at the desired zoom. May be Loading/Failed.
    /// Aligned with cesium-native _pLoadingTile and retains tile lifetime.
    std::shared_ptr<RasterOverlayTile> _pLoadingTile;

    /// The tile we RENDER (always Loaded/Done, texture available).
    /// Aligned with cesium-native _pReadyTile and retains texture lifetime.
    std::shared_ptr<RasterOverlayTile> _pReadyTile;

    /// Cached pointer to the ready texture (from _pReadyTile->getTexture()).
    Texture* readyTexture_ = nullptr;

    ReadyTileSource loadingTileSource_ = ReadyTileSource::None;
    ReadyTileSource readyTileSource_ = ReadyTileSource::None;
    SourceTileList mappedSourceTiles_;
    bool directRasterTile_ = false;

    /// UV transform: rasterUV = geometryUV * scale + offset.
    float offsetU_ = 0.0f, offsetV_ = 0.0f;
    float scaleU_ = 1.0f, scaleV_ = 1.0f;

    /// 目标几何缓存(闸3)。见上方 targetGeometryCacheValid/cacheTargetGeometry。
    bool targetGeometryCached_ = false;
    const void* cachedDetailsPtr_ = nullptr;
    RasterOverlayProjection cachedProjection_ =
        RasterOverlayProjection::Geographic;
    double cachedTargetScreenPixelsX_ = 0.0;
    double cachedTargetScreenPixelsY_ = 0.0;
    std::optional<Rectangle> cachedBoundingVolumeRectangle_;

    /// update() 最近一次实际用于 computeTranslationAndScale 的几何矩形。
    /// 供 promoteLoadedTileWithoutGeometryWork 在无 update() 的瓦片上复算 UV。
    std::optional<Rectangle> lastMappingRectangle_;
    bool lastMappingRectangleInvertedV_ = false;

    /// Set once when the original desired-zoom tile fails.
    /// Never cleared — suppresses MoreDetailAvailable::Yes.
    /// Aligned with cesium-native _originalFailed.
    bool originalFailed_ = false;

    /// The geometry tile key this raster is mapped to.
    /// Used for attachRaster/detachRaster notification key consistency.
    TileKey geometryKey_;

    /// Raster-overlay texture coordinate index for this projection.
    /// Aligned with cesium-native _textureCoordinateID.
    int32_t textureCoordinateID_ = -1;

    /// Our layer slot in the owning tile's rasterOverlays vector.
    /// This is resource notification identity, not a texture-coordinate ID.
    int32_t overlaySlot_ = 0;
};

} // namespace earth_engine
