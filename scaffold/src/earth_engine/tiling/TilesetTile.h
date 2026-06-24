#pragma once

#include "TileKey.h"
#include "TileLoadState.h"
#include "TilePlan.h"
#include "TileContentStateTransition.h"
#include "TileContentRuntimeState.h"
#include "TileRasterOverlayState.h"
#include "TileRefine.h"
#include "TileRenderablePolicy.h"
#include "TileRenderReferenceState.h"
#include "TileRenderContentState.h"
#include "TileSelectionFrameState.h"
#include "TileBoundingVolume.h"
#include "../core/math/MathUtils.h"
#include "../core/math/Rectangle.h"
#include "../providers/TerrainProvider.h"
#include "TileScheme.h"

#include <memory>
#include <vector>
#include <cstdint>
#include <optional>

namespace earth_engine {

class RasterMappedToTilesetTile;

/// cesium-native Tile equivalent.
/// Each tile in the unified quadtree holds geometry (QM mesh) and
/// raster overlays (imagery). The tree uses GeographicTMSScheme.
struct TilesetTile {
    TileKey key;
    Rectangle bounds;          // geographic radians
    double geometricError = 0.0;
    TileRefine refine = TileRefine::Replace;
    bool unconditionallyRefine = false;
    std::optional<TileBoundingVolume> boundingVolume;
    std::optional<TileBoundingVolume> viewerRequestVolume;
    std::optional<TileBoundingVolume> contentBoundingVolume;
    std::optional<TileBoundingVolume> initialBoundingVolume;
    std::optional<TileBoundingVolume> initialContentBoundingVolume;
    bool contentProviderTerrainQuadtreeTile = false;

    // ---- Tree structure (cesium-native parent/child) ----
    // DIFF from cesium-native: children are raw pointers, ownership is in
    // Tileset::tiles_ flat map (not tree-owned). This enables O(1) key-based
    // lookup for the unload queue and terrain cache. Behaviorally equivalent
    // for quadtree traversal and cascading eviction.
    TilesetTile* parent = nullptr;
    std::vector<TilesetTile*> children;  // raw pointers, owned by Tileset::tiles_ map

    /// cesium-native: getChildren() — returns view of child tiles.
    const std::vector<TilesetTile*>& getChildren() const { return children; }
    std::vector<TilesetTile*>& getChildren() { return children; }

    /// cesium-native: getParent() — returns parent tile (may be nullptr).
    TilesetTile* getParent() { return parent; }
    const TilesetTile* getParent() const { return parent; }

    double nonZeroGeometricError() const {
        if (geometricError > MathUtils::Epsilon5) {
            return geometricError;
        }

        const TilesetTile* ancestor = parent;
        double divisor = 1.0;
        while (ancestor) {
            if (!ancestor->unconditionallyRefine) {
                divisor *= 2.0;
                if (ancestor->geometricError > MathUtils::Epsilon5) {
                    return ancestor->geometricError / divisor;
                }
            }
            ancestor = ancestor->parent;
        }

        return MathUtils::Epsilon5;
    }

    // ---- Content (terrain mesh / GPU buffers / glTF render resources) ----
    TileContentRuntimeState content;

    void markContentLoading() {
        TileContentStateTransition::markLoading(
            content.loadState,
            content.contentKind);
    }

    void markRenderContentLoaded() {
        TileContentStateTransition::markRenderLoaded(
            content.loadState,
            content.contentKind);
    }

    void markRenderContentDone() {
        TileContentStateTransition::markRenderDone(
            content.renderContent,
            content.loadState,
            content.contentKind);
    }

    template <typename IsCompleteRenderableFn>
    void commitSurfaceRenderContent(SurfaceDrawableSource source,
                                    bool markDone,
                                    IsCompleteRenderableFn&&
                                        isCompleteRenderable) {
        content.renderContent.setTerrainRenderContent(true);
        content.renderContent.setMeshReady(true);
        content.renderContent.setSurfaceSource(source);
        content.contentKind = TileContentKind::Render;
        if (markDone) {
            content.loadState = TileLoadState::Done;
        }
        updateFrameRenderability(
            hasSurfaceDrawable(),
            isCompleteRenderable(*this));
    }

    void refreshSurfaceDrawable(bool drawable) {
        content.renderContent.setSurfaceDrawable(drawable);
    }

    void updateFrameRenderability(bool drawable, bool complete) {
        content.renderContent.setSurfaceDrawable(drawable);
        selectionFrameState.updateFrameRenderability(complete);
    }

    void clearFrameRenderability() {
        selectionFrameState.clearFrameRenderability();
    }

    void updateTraversalRenderability(bool traversalRenderable) {
        selectionFrameState.updateTraversalRenderability(traversalRenderable);
    }

    TileRenderableSnapshot renderableSnapshot(
        bool requiredRasterOverlaysReady) const {
        const bool renderContentReady =
            contentProviderTerrainQuadtreeTile &&
                    !content.renderContent.hasGltfContent()
                ? false
                : content.renderContent.isRenderContentReady();
        return TileRenderableSnapshot{
            content.loadState,
            content.contentKind,
            unconditionallyRefine,
            !children.empty(),
            requiredRasterOverlaysReady,
            renderContentReady};
    }

    bool canPrepareRasterOverlays() const {
        return content.loadState == TileLoadState::Done &&
               content.contentKind == TileContentKind::Render &&
               hasRasterOverlayHostContent() &&
               content.renderContent.isRenderContentReady();
    }

    bool hasRasterOverlayHostContent() const {
        if (contentProviderTerrainQuadtreeTile &&
            !content.renderContent.hasGltfContent()) {
            return false;
        }
        return content.renderContent.hasRenderableTerrainContent();
    }

    bool waitsForContentTerrainRasterDetails() const {
        return contentProviderTerrainQuadtreeTile &&
               !(content.contentKind == TileContentKind::Render &&
                 content.renderContent.hasRasterOverlayDetailsContent());
    }

    bool hasSurfaceDrawable() const {
        return TileRenderablePolicy::hasSurfaceDrawable(
            content.contentKind,
            content.loadState,
            content.renderContent.isMeshReady(),
            content.renderContent.surfaceVertexBuffer() != nullptr);
    }

    void markRenderContentFailedTemporarily() {
        TileContentStateTransition::markRenderFailedTemporarily(
            content.renderContent,
            content.loadState,
            content.contentKind);
    }

    void markContentFailedTemporarily() {
        TileContentStateTransition::markFailedTemporarily(
            content.loadState,
            content.contentKind);
    }

    void markContentFailedPermanently() {
        TileContentStateTransition::markFailedPermanently(
            content.loadState,
            content.contentKind);
    }

    void markEmptyContentLoaded() {
        TileContentStateTransition::markEmptyLoaded(
            content.loadState,
            content.contentKind);
    }

    void markEmptyContentDone() {
        TileContentStateTransition::markEmptyDone(
            content.loadState,
            content.contentKind);
    }

    void markExternalContentDone() {
        TileContentStateTransition::markExternalDone(
            content.loadState,
            content.contentKind,
            unconditionallyRefine);
    }

    void markContentUnloading() {
        TileContentStateTransition::markUnloading(content.loadState);
    }

    void markContentUnloaded() {
        TileContentStateTransition::markUnloaded(
            content.loadState,
            content.contentKind,
            selectionFrameState);
    }

    // ── cesium-native reference counting (Tile::getReferenceCount) ──
    /// Incremented when tile content is referenced by the renderer.
    /// Tiles with referenceCount > 0 are ineligible for unloading.
    int32_t referenceCount() const { return renderReferences.count(); }
    uint64_t lastUsedFrame() const { return renderReferences.lastUsedFrame(); }
    void markUsedForRenderFrame(uint64_t frameNumber) {
        renderReferences.markUsedForFrame(frameNumber);
    }
    void addReference() { renderReferences.add(); }
    void removeReference() { renderReferences.remove(); }
    void clearReferences() { renderReferences.clear(); }
    TileRenderReferenceState renderReferences;

    // ---- Raster overlays ----
    TileRasterOverlayState rasterOverlayState;

    // ---- Selection / LOD frame state ----
    TileSelectionFrameState selectionFrameState;

    TilesetTile() = default;
    TilesetTile(TileKey k, Rectangle b, TilesetTile* p = nullptr)
        : key(std::move(k)), bounds(b), parent(p) {}
    TilesetTile(const TilesetTile&) = delete;
    TilesetTile& operator=(const TilesetTile&) = delete;
    TilesetTile(TilesetTile&&) = delete;
    TilesetTile& operator=(TilesetTile&&) = delete;

    /// cesium-native: create 4 child tiles.
    ///
    /// DIFF from cesium-native: this method remains a no-op for API
    /// alignment. Children are materialized by the stateful selector through
    /// Tileset::ensureTileChildren when traversal decides refinement is needed.
    void createChildren(const TileScheme& scheme) {
        (void)scheme;
    }

    /// Find the deepest ancestor with a ready mesh (for upsampling)
    TilesetTile* findUpsampleAncestor() {
        TilesetTile* p = parent;
        while (p) {
            if (p->content.renderContent.isTerrainRenderContent() &&
                p->content.renderContent.hasGltfContent()) {
                if (p->content.renderContent.isGltfRenderReady()) {
                    return p;
                }
                p = p->parent;
                continue;
            }
            if (p->content.renderContent.isSurfaceMeshReady() &&
                p->content.renderContent.surfaceVertexBuffer()) return p;
            p = p->parent;
        }
        return nullptr;
    }
    const TilesetTile* findUpsampleAncestor() const {
        const TilesetTile* p = parent;
        while (p) {
            if (p->content.renderContent.isTerrainRenderContent() &&
                p->content.renderContent.hasGltfContent()) {
                if (p->content.renderContent.isGltfRenderReady()) {
                    return p;
                }
                p = p->parent;
                continue;
            }
            if (p->content.renderContent.isSurfaceMeshReady() &&
                p->content.renderContent.surfaceVertexBuffer()) return p;
            p = p->parent;
        }
        return nullptr;
    }
};

} // namespace earth_engine
