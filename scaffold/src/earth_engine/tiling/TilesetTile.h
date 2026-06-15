#pragma once

#include "TileKey.h"
#include "TilePlan.h"
#include "TileRefine.h"
#include "TileBoundingVolume.h"
#include "../content/GltfModel.h"
#include "../core/math/Mat4.h"
#include "../core/math/Rectangle.h"
#include "../tiling/SurfaceTile.h"
#include "../providers/TerrainProvider.h"
#include "../renderer/RenderDevice.h"
#include "TileScheme.h"

#include <memory>
#include <vector>
#include <cstdint>
#include <optional>
#include <array>

namespace earth_engine {

class RasterMappedToTilesetTile;

enum class TileLoadState {
    Unloading = -2,
    FailedTemporarily = -1,
    Unloaded = 0,
    ContentLoading = 1,
    ContentLoaded = 2,
    Done = 3,
    Failed = 4
};

enum class TileContentKind {
    Unknown,
    Empty,
    External,
    Render
};

/// Renderer-side resources for one glTF mesh primitive.
/// This mirrors cesium-native TileRenderContent::getRenderResources without
/// mixing platform buffers into the parsed glTF model data.
struct GltfPrimitiveRenderResources {
    std::unique_ptr<Buffer> vertexBuffer;
    std::unique_ptr<Buffer> indexBuffer;
    std::unique_ptr<Buffer> instanceBuffer;
    struct TextureBinding {
        Texture* texture = nullptr;
        int texCoord = 0;
        std::array<float, 4> offsetScale = {0.0f, 0.0f, 1.0f, 1.0f};
        std::array<float, 2> rotationSinCos = {0.0f, 1.0f};
    };
    TextureBinding baseColorTexture;
    TextureBinding metallicRoughnessTexture;
    TextureBinding specularTexture;
    TextureBinding specularColorTexture;
    TextureBinding clearcoatTexture;
    TextureBinding clearcoatRoughnessTexture;
    TextureBinding clearcoatNormalTexture;
    TextureBinding sheenColorTexture;
    TextureBinding sheenRoughnessTexture;
    TextureBinding normalTexture;
    TextureBinding occlusionTexture;
    TextureBinding emissiveTexture;
    int vertexCount = 0;
    int indexCount = 0;
    int instanceCount = 0;
    GltfPrimitiveMode primitiveMode = GltfPrimitiveMode::Triangles;
    Vec3 sortCenterEcef = Vec3::zero();
    uint64_t animationRevision = 0;
    std::array<float, 4> baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float dielectricSpecularF0 = 0.04f;
    float specularFactor = 1.0f;
    std::array<float, 3> specularColorFactor = {1.0f, 1.0f, 1.0f};
    float clearcoatFactor = 0.0f;
    float clearcoatRoughnessFactor = 0.0f;
    float clearcoatNormalTextureScale = 1.0f;
    std::array<float, 3> sheenColorFactor = {0.0f, 0.0f, 0.0f};
    float sheenRoughnessFactor = 0.0f;
    float normalTextureScale = 1.0f;
    float occlusionTextureStrength = 1.0f;
    std::array<float, 3> emissiveFactor = {0.0f, 0.0f, 0.0f};
    GltfAlphaMode alphaMode = GltfAlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    bool unlit = false;
    bool dynamicVertices = false;
};

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

    // ---- Content (geometry) ----
    /// QM raw binary data (lazily decoded to SurfaceTileMesh)
    std::unique_ptr<DecodedHeightmap> heightmap;
    /// Parsed QM mesh (null until decoded)
    std::unique_ptr<SurfaceTileMesh> mesh;
    /// cesium-native TileRenderContent analogue for glTF/3D Tiles content.
    std::unique_ptr<GltfModel> gltfModel;
    Mat4 gltfContentTransform = Mat4::identity();
    std::vector<std::unique_ptr<Texture>> gltfTextureResources;
    std::vector<GltfPrimitiveRenderResources> gltfPrimitiveResources;
    /// GPU vertex buffer for this tile's geometry
    std::unique_ptr<Buffer> gpuVertexBuffer;
    /// GPU index buffer (per-tile, from QM triangulation)
    std::unique_ptr<Buffer> gpuIndexBuffer;
    /// Tile-local origin (for RTC: positions are ECEF - origin)
    Vec3 localOrigin = Vec3::zero();
    TileLoadState loadState = TileLoadState::Unloaded;
    TileContentKind contentKind = TileContentKind::Unknown;
    /// cesium-native updated BoundingRegion height range after terrain load.
    bool hasTerrainHeightRange = false;
    double terrainMinimumHeight = 0.0;
    double terrainMaximumHeight = 0.0;
    /// Whether the mesh is ready for rendering
    bool meshReady = false;
    /// cesium-native UpsampledQuadtreeNode equivalent. The tile is not
    /// requestable; its render content is derived from an ancestor tile.
    bool upsampledFromParent = false;
    /// Last frame this tile was used (for LRU eviction)
    uint64_t lastUsedFrame = 0;

    // ── cesium-native reference counting (Tile::getReferenceCount) ──
    /// Incremented when tile content is referenced by the renderer.
    /// Tiles with referenceCount > 0 are ineligible for unloading.
    int32_t referenceCount() const { return _referenceCount; }
    void addReference() { ++_referenceCount; }
    void removeReference() { --_referenceCount; if (_referenceCount < 0) _referenceCount = 0; }
    void clearReferences() { _referenceCount = 0; }
    int32_t _referenceCount = 0;

    // ---- Raster overlays ----
    std::vector<std::unique_ptr<RasterMappedToTilesetTile>> rasterOverlays;
    std::vector<RasterOverlayProjection> missingRasterOverlayProjections;

    // ---- LOD state ----
    bool renderable = false;
    bool subdivisionDesired = false;
    TileSelectionState previousSelectionState = TileSelectionState::NotVisited;
    TileSelectionState selectionState = TileSelectionState::NotVisited;
    double screenSpaceError = 0.0;
    bool inFrustum = false;
    bool cameraInside = false;
    bool ancestorMeetsSse = false;
    float lodTransitionFadePercentage = 1.0f;

    TilesetTile() = default;
    TilesetTile(TileKey k, Rectangle b, TilesetTile* p = nullptr)
        : key(std::move(k)), bounds(b), parent(p) {}

    /// cesium-native: create 4 child tiles.
    ///
    /// DIFF from cesium-native: children are created lazily in
    /// Tileset::buildRenderCommands (when they enter the view frustum),
    /// not eagerly when the parent is loaded. This is a memory optimization
    /// for mobile platforms — only visible tiles are allocated.
    /// This method exists for API alignment; the actual child creation
    /// logic is in buildRenderCommands (parent↔child linking + bounds init).
    void createChildren(const TileScheme& scheme) {
        (void)scheme;
        // Children are materialized in Tileset::buildRenderCommands
        // when they become visible. See the parent-chain creation logic there.
    }

    /// Find the deepest ancestor with a ready mesh (for upsampling)
    TilesetTile* findUpsampleAncestor() {
        TilesetTile* p = parent;
        while (p) {
            if (p->meshReady && p->gpuVertexBuffer) return p;
            p = p->parent;
        }
        return nullptr;
    }
    const TilesetTile* findUpsampleAncestor() const {
        const TilesetTile* p = parent;
        while (p) {
            if (p->meshReady && p->gpuVertexBuffer) return p;
            p = p->parent;
        }
        return nullptr;
    }
};

} // namespace earth_engine
