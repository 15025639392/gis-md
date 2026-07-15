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
#include "TileRetryBackoffPolicy.h"
#include "TileRenderContentState.h"
#include "TileSelectionFrameState.h"
#include "TileBoundingVolume.h"
#include "../core/math/MathUtils.h"
#include "../core/math/Rectangle.h"

#include <algorithm>
#include <memory>
#include <vector>
#include <cstdint>
#include <optional>

namespace earth_engine {

class RasterMappedToTilesetTile;
class TileScheme;

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

    bool attachChild(TilesetTile& child) {
        if (std::find(children.begin(), children.end(), &child) !=
            children.end()) {
            if (child.parent != this) {
                child.parent = this;
                child.invalidateChildMaterialization();
                invalidateChildMaterialization();
            }
            return false;
        }
        child.parent = this;
        child.invalidateChildMaterialization();
        children.push_back(&child);
        invalidateChildMaterialization();
        return true;
    }

    void clearChildren() {
        if (children.empty()) {
            return;
        }
        for (TilesetTile* child : children) {
            if (child && child->parent == this) {
                child->parent = nullptr;
                child->invalidateChildMaterialization();
            }
        }
        children.clear();
        invalidateChildMaterialization();
    }

    void setBounds(const Rectangle& value) {
        if (bounds == value) {
            return;
        }
        bounds = value;
        notifyChildMaterializationStateChanged();
    }

    void setGeometricError(double value) {
        if (geometricError == value) {
            return;
        }
        geometricError = value;
        notifyChildMaterializationStateChanged();
    }

    void setRefine(TileRefine value) {
        if (refine == value) {
            return;
        }
        refine = value;
        notifyChildMaterializationStateChanged();
    }

    void setUnconditionallyRefine(bool value) {
        if (unconditionallyRefine == value) {
            return;
        }
        unconditionallyRefine = value;
        notifyChildMaterializationStateChanged();
    }

    void setBoundingVolume(std::optional<TileBoundingVolume> value) {
        boundingVolume = std::move(value);
        notifyChildMaterializationStateChanged();
    }

    void setContentBoundingVolume(
        std::optional<TileBoundingVolume> value) {
        contentBoundingVolume = std::move(value);
        notifyChildMaterializationStateChanged();
    }

    void resetContentBoundingVolume() {
        if (!contentBoundingVolume) {
            return;
        }
        contentBoundingVolume.reset();
        notifyChildMaterializationStateChanged();
    }

    void invalidateChildMaterialization() {
        ++childMaterializationInputRevision;
        if (childMaterializationInputRevision == 0) {
            childMaterializationInputRevision = 1;
        }
        childMaterializationStateValid = false;
    }

    void notifyChildMaterializationStateChanged() {
        invalidateChildMaterialization();
        if (parent) {
            parent->invalidateChildMaterialization();
        }
    }

    void markTerrainAvailabilityUpsample() {
        if (content.isTerrainAvailabilityUpsample() &&
            !content.rasterDetailSourceProjection) {
            return;
        }
        content.markTerrainAvailabilityUpsample();
        notifyChildMaterializationStateChanged();
    }

    void markRasterDetailUpsample() {
        if (content.isRasterDetailUpsample() &&
            !content.rasterDetailSourceProjection) {
            return;
        }
        content.markRasterDetailUpsample();
        notifyChildMaterializationStateChanged();
    }

    void markRasterDetailUpsample(
        RasterOverlayProjection sourceProjection) {
        if (content.isRasterDetailUpsample() &&
            content.rasterDetailSourceProjection == sourceProjection) {
            return;
        }
        content.markRasterDetailUpsample(sourceProjection);
        notifyChildMaterializationStateChanged();
    }

    void clearUpsampleKind() {
        if (!content.derivesTerrainFromParent() &&
            !content.rasterDetailSourceProjection) {
            return;
        }
        content.clearUpsampleKind();
        notifyChildMaterializationStateChanged();
    }

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

    // 瞬时失败退避:避免 FailedTemporarily 瓦片每帧重打服务器(见
    // TileRetryBackoffPolicy)。标记临时失败时 record,成功加载时 reset。
    double temporaryFailureRetryNotBeforeMs = 0.0;
    int temporaryFailureCount = 0;

    void recordTemporaryFailureBackoff(double nowMs) {
        ++temporaryFailureCount;
        temporaryFailureRetryNotBeforeMs =
            nowMs + TileRetryBackoffPolicy::backoffMs(temporaryFailureCount);
    }
    void resetTemporaryFailureBackoff() {
        temporaryFailureCount = 0;
        temporaryFailureRetryNotBeforeMs = 0.0;
    }

    void markContentLoading() {
        const TileLoadState previousLoadState = content.loadState;
        const TileContentKind previousContentKind = content.contentKind;
        TileContentStateTransition::markLoading(
            content.loadState,
            content.contentKind);
        if (content.loadState != previousLoadState ||
            content.contentKind != previousContentKind) {
            notifyChildMaterializationStateChanged();
        }
    }

    void markRenderContentLoaded() {
        const TileLoadState previousLoadState = content.loadState;
        const TileContentKind previousContentKind = content.contentKind;
        resetTemporaryFailureBackoff();
        TileContentStateTransition::markRenderLoaded(
            content.loadState,
            content.contentKind);
        if (content.loadState != previousLoadState ||
            content.contentKind != previousContentKind) {
            notifyChildMaterializationStateChanged();
        }
    }

    void markRenderContentDone() {
        const TileLoadState previousLoadState = content.loadState;
        const TileContentKind previousContentKind = content.contentKind;
        const bool wasReady =
            content.renderContent.isRenderContentReady();
        TileContentStateTransition::markRenderDone(
            content.renderContent,
            content.loadState,
            content.contentKind);
        if (content.loadState != previousLoadState ||
            content.contentKind != previousContentKind ||
            content.renderContent.isRenderContentReady() != wasReady) {
            notifyChildMaterializationStateChanged();
        }
    }

    template <typename IsCompleteRenderableFn>
    void commitSurfaceRenderContent(SurfaceDrawableSource source,
                                    bool markDone,
                                    IsCompleteRenderableFn&&
                                        isCompleteRenderable) {
        resetTemporaryFailureBackoff();
        content.renderContent.setTerrainRenderContent(true);
        content.renderContent.setMeshReady(true);
        content.renderContent.setSurfaceSource(source);
        content.contentKind = TileContentKind::Render;
        if (markDone) {
            content.loadState = TileLoadState::Done;
        }
        notifyChildMaterializationStateChanged();
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
        return TileRenderableSnapshot{
            content.loadState,
            content.contentKind,
            unconditionallyRefine,
            !children.empty(),
            requiredRasterOverlaysReady,
            content.renderContent.isRenderContentReady()};
    }

    bool canPrepareRasterOverlays() const {
        return content.loadState == TileLoadState::Done &&
               content.contentKind == TileContentKind::Render &&
               hasRasterOverlayHostContent() &&
               content.renderContent.isRenderContentReady();
    }

    bool hasCommittedRenderContent() const {
        return content.contentKind == TileContentKind::Render &&
               (content.loadState == TileLoadState::ContentLoaded ||
                content.loadState == TileLoadState::Done);
    }

    bool hasRasterOverlayHostContent() const {
        return content.renderContent.hasRenderableTerrainContent();
    }

    bool waitsForContentTerrainRasterDetails() const {
        if (content.contentKind != TileContentKind::Render ||
            !content.renderContent.isTerrainRenderContent()) {
            return false;
        }
        // ContentLoading tiles always wait — the glTF residue may have
        // stale overlay details from a previous load cycle.
        if (content.loadState == TileLoadState::ContentLoading) {
            return true;
        }
        return (content.loadState == TileLoadState::ContentLoaded ||
                content.loadState == TileLoadState::Done) &&
               !content.renderContent.hasRasterOverlayDetailsContent();
    }

    bool hasSurfaceDrawable() const {
        return content.renderContent.hasGltfContent() &&
               content.renderContent.isRenderContentReady();
    }

    void markRenderContentFailedTemporarily() {
        const TileLoadState previousLoadState = content.loadState;
        const TileContentKind previousContentKind = content.contentKind;
        TileContentStateTransition::markRenderFailedTemporarily(
            content.renderContent,
            content.loadState,
            content.contentKind);
        if (content.loadState != previousLoadState ||
            content.contentKind != previousContentKind) {
            notifyChildMaterializationStateChanged();
        }
    }

    void markContentFailedTemporarily() {
        const TileLoadState previousLoadState = content.loadState;
        const TileContentKind previousContentKind = content.contentKind;
        TileContentStateTransition::markFailedTemporarily(
            content.loadState,
            content.contentKind);
        if (content.loadState != previousLoadState ||
            content.contentKind != previousContentKind) {
            notifyChildMaterializationStateChanged();
        }
    }

    void markContentFailedPermanently() {
        const TileLoadState previousLoadState = content.loadState;
        const TileContentKind previousContentKind = content.contentKind;
        TileContentStateTransition::markFailedPermanently(
            content.loadState,
            content.contentKind);
        if (content.loadState != previousLoadState ||
            content.contentKind != previousContentKind) {
            notifyChildMaterializationStateChanged();
        }
    }

    void markEmptyContentLoaded() {
        const TileLoadState previousLoadState = content.loadState;
        const TileContentKind previousContentKind = content.contentKind;
        resetTemporaryFailureBackoff();
        TileContentStateTransition::markEmptyLoaded(
            content.loadState,
            content.contentKind);
        if (content.loadState != previousLoadState ||
            content.contentKind != previousContentKind) {
            notifyChildMaterializationStateChanged();
        }
    }

    void markEmptyContentDone() {
        const TileLoadState previousLoadState = content.loadState;
        const TileContentKind previousContentKind = content.contentKind;
        TileContentStateTransition::markEmptyDone(
            content.loadState,
            content.contentKind);
        if (content.loadState != previousLoadState ||
            content.contentKind != previousContentKind) {
            notifyChildMaterializationStateChanged();
        }
    }

    void markExternalContentDone() {
        const TileLoadState previousLoadState = content.loadState;
        const TileContentKind previousContentKind = content.contentKind;
        const bool wasUnconditionallyRefined = unconditionallyRefine;
        TileContentStateTransition::markExternalDone(
            content.loadState,
            content.contentKind,
            unconditionallyRefine);
        if (content.loadState != previousLoadState ||
            content.contentKind != previousContentKind ||
            unconditionallyRefine != wasUnconditionallyRefined) {
            notifyChildMaterializationStateChanged();
        }
    }

    void markContentUnloading() {
        const TileLoadState previousLoadState = content.loadState;
        TileContentStateTransition::markUnloading(content.loadState);
        if (content.loadState != previousLoadState) {
            notifyChildMaterializationStateChanged();
        }
    }

    void markContentUnloaded() {
        const TileLoadState previousLoadState = content.loadState;
        const TileContentKind previousContentKind = content.contentKind;
        TileContentStateTransition::markUnloaded(
            content.loadState,
            content.contentKind,
            selectionFrameState);
        if (content.loadState != previousLoadState ||
            content.contentKind != previousContentKind) {
            notifyChildMaterializationStateChanged();
        }
    }

    // ── cesium-native reference counting (Tile::getReferenceCount) ──
    /// Shared by selector traversal ownership and renderer submit ownership.
    /// Tiles with referenceCount > 0 are ineligible for unloading.
    int32_t referenceCount() const { return referenceState.count(); }
    uint64_t lastUsedFrame() const { return referenceState.lastUsedFrame(); }
    void markUsedForRenderFrame(uint64_t frameNumber) {
        referenceState.markUsedForFrame(frameNumber);
    }
    void addReference() { referenceState.add(); }
    void removeReference() { referenceState.remove(); }
    void clearReferences() { referenceState.clear(); }
    TileRenderReferenceState referenceState;

    // ---- Raster overlays ----
    TileRasterOverlayState rasterOverlayState;

    // ---- Selection / LOD frame state ----
    TileSelectionFrameState selectionFrameState;

    // Incremental selection reset stamp: the selector frame id at which this
    // tile's selectionFrameState was last reset/visited. Guards both
    // once-per-frame reset and the previous-traversal history decay.
    // Replaces the old per-frame full-registry reset sweep.
    uint64_t selectionActiveFrameId = 0;
    // The selector frame in which this tile was added to the current traversal
    // ownership set. Kept separate from selectionActiveFrameId because a tile
    // may be reset from previous-traversal history before it is visited again.
    uint64_t selectionTraversalFrameId = 0;

    // Child creation follows cesium-native's state-transition model. Provider
    // topology changes and explicit tile mutations invalidate this generation;
    // stable selection frames compare only integer versions.
    uint64_t childMaterializationInputRevision = 1;
    uint64_t appliedChildMaterializationInputRevision = 0;
    uint64_t appliedChildTopologyRevision = 0;
    uint64_t appliedChildMaterializationConfiguration = 0;
    bool childMaterializationStateValid = false;

    // Provider metadata is immutable by default. Providers that explicitly
    // publish a metadata revision may refresh an existing Unloaded tile once
    // per revision.
    uint64_t appliedContentMetadataRevision = 0;

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

};

} // namespace earth_engine
