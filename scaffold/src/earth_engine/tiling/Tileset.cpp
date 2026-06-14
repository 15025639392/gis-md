#include "Tileset.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../renderer/Renderer.h"
#include "../renderer/RenderDevice.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Cartographic.h"
#include "../tiling/TileSurface.h"
#include "../terrain/QuantizedMeshParser.h"
#include "../providers/QuantizedMeshTerrainProvider.h"
#include "../terrain/TerrainTile.h"
#include "../layers/BasemapLayer.h"
#include "../debug/PerfTimer.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace earth_engine {

Tileset::Tileset(std::unique_ptr<TerrainProvider> terrainProvider,
                 std::unique_ptr<TileScheme> tileScheme,
                 std::vector<BasemapLayer*> imageryLayers,
                 RenderDevice* device)
    : terrainProvider_(std::move(terrainProvider)),
      tileScheme_(std::move(tileScheme)),
      imageryLayers_(std::move(imageryLayers)),
      device_(device),
      quadTree_(std::make_unique<TileQuadTree>()) {}

Tileset::~Tileset() = default;

std::string Tileset::terrainCacheKey(const TileKey& key) const {
    return key.schemeId + "/" + std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" + std::to_string(key.y);
}

void Tileset::update(const FrameState& frameState) {
    if (!frameState.camera) return;

    // Track camera movement
    const Vec3 camPos = frameState.camera->position();
    if (lastCameraPosition_.lengthSquared() > 0.0) {
        cameraMoving_ = camPos.distanceTo(lastCameraPosition_) > 2.0;
    }
    lastCameraPosition_ = camPos;

    // LOD selection via shared quadtree
    tilePlan_ = quadTree_->compute(
        *frameState.camera, *tileScheme_,
        static_cast<double>(frameState.viewportWidthPixels),
        static_cast<double>(frameState.viewportHeightPixels));

    // Request missing terrain tiles
    requestMissingTiles(tilePlan_.visibleTiles);

    // Process completed terrain tile requests
    processPendingUploads();
}

void Tileset::requestMissingTiles(const std::vector<TileKey>& visibleKeys) {
    const int maxRequests = cameraMoving_ ? 2 : 4;
    const size_t maxInflight = cameraMoving_ ? 96 : 128;
    int issued = 0;

    for (const TileKey& key : visibleKeys) {
        if (issued >= maxRequests) break;
        if (pendingRequests_.size() >= maxInflight) break;

        TileKey requestKey = key;
        while (requestKey.z > terrainProvider_->maxZoom()) {
            requestKey = TilePlanBuilder::parentKey(requestKey);
        }
        if (requestKey.z < terrainProvider_->minZoom()) continue;

        std::string ck = terrainCacheKey(requestKey);
        if (terrainCache_.count(ck)) continue;
        if (pendingRequests_.count(ck)) continue;
        if (emptyTiles_.count(ck)) continue;
        if (!terrainProvider_->supportsTile(requestKey)) continue;

        pendingRequests_.insert(ck);
        ++issued;

        auto* provider = terrainProvider_.get();
        CancellationToken token;
        provider->requestTile(requestKey, token,
            [this, ck](const TileKey&, std::unique_ptr<DecodedHeightmap> hm) {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                if (hm) {
                    pendingUploads_.push_back({ck, std::move(hm)});
                } else {
                    emptyTiles_.insert(ck);
                }
                pendingRequests_.erase(ck);
            });
    }
}

void Tileset::processPendingUploads() {
    std::vector<std::pair<std::string, std::unique_ptr<DecodedHeightmap>>> batch;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        const size_t kMax = cameraMoving_ ? 1 : 2;
        while (!pendingUploads_.empty() && batch.size() < kMax) {
            batch.push_back(std::move(pendingUploads_.front()));
            pendingUploads_.pop_front();
        }
    }
    for (auto& [ck, hm] : batch) {
        terrainCache_[ck] = std::move(hm);
    }
}

void Tileset::ensureTileMesh(TilesetTile& tile) {
    if (tile.meshReady) return;

    auto it = terrainCache_.find(terrainCacheKey(tile.key));
    if (it == terrainCache_.end()) return;

    const auto& hm = it->second;
    if (!hm->rawData.empty()) {
        tile.mesh = QuantizedMeshParser::parseToSurfaceTileMesh(
            hm->rawData.data(), hm->rawData.size(), tile.bounds);
    }
    if (!tile.mesh) {
        // cesium-native: build regular grid mesh with terrain heights.
        // Uses the DecodedHeightmap from terrain cache for height sampling.
        tile.mesh = std::make_unique<SurfaceTileMesh>();
        *tile.mesh = TileSurface::buildEllipsoidMesh(tile.bounds, 64);
        if (hm->valid()) {
            const auto& ellipsoid = Ellipsoid::WGS84();
            // Create a temporary TerrainTile for height sampling
            TerrainTile tempTile(tile.key, *tileScheme_,
                std::make_unique<DecodedHeightmap>(*hm));
            for (auto& v : tile.mesh->vertices) {
                Cartographic c = ellipsoid.cartesianToCartographic(v.positionEcef);
                double h = static_cast<double>(tempTile.sampleHeight(
                    c.longitude(), c.latitude()));
                Cartographic tc = Cartographic::fromRadians(
                    c.longitude(), c.latitude(), h);
                v.positionEcef = ellipsoid.cartographicToCartesian(tc);
            }
        }
    }

    tile.localOrigin = Vec3::zero();
    if (!tile.mesh->vertices.empty()) {
        for (const auto& v : tile.mesh->vertices) {
            tile.localOrigin += v.positionEcef;
        }
        tile.localOrigin = tile.localOrigin / static_cast<double>(tile.mesh->vertices.size());
    }

    if (device_ && !tile.mesh->vertices.empty()) {
        struct GpuVertex { float pos[3]; float nrm[3]; float uv[2]; };
        std::vector<GpuVertex> verts(tile.mesh->vertices.size());
        for (size_t i = 0; i < tile.mesh->vertices.size(); ++i) {
            const auto& src = tile.mesh->vertices[i];
            Vec3 rel = src.positionEcef - tile.localOrigin;
            verts[i].pos[0] = static_cast<float>(rel.x());
            verts[i].pos[1] = static_cast<float>(rel.y());
            verts[i].pos[2] = static_cast<float>(rel.z());
            Vec3 nrm = src.normalEcef;
            if (nrm.lengthSquared() > 0.0) nrm = nrm.normalized();
            else nrm = Ellipsoid::WGS84().geodeticSurfaceNormal(src.positionEcef);
            verts[i].nrm[0] = static_cast<float>(nrm.x());
            verts[i].nrm[1] = static_cast<float>(nrm.y());
            verts[i].nrm[2] = static_cast<float>(nrm.z());
            verts[i].uv[0] = src.uv[0];
            verts[i].uv[1] = src.uv[1];
        }
        BufferDesc vbDesc;
        vbDesc.size = verts.size() * sizeof(GpuVertex);
        vbDesc.data = verts.data();
        vbDesc.usage = BufferDesc::Usage::Static;
        vbDesc.type = BufferDesc::Type::Vertex;
        tile.gpuVertexBuffer = device_->createBuffer(vbDesc);

        if (!tile.mesh->indices.empty()) {
            BufferDesc ibDesc;
            ibDesc.size = tile.mesh->indices.size() * sizeof(uint32_t);
            ibDesc.data = tile.mesh->indices.data();
            ibDesc.usage = BufferDesc::Usage::Static;
            ibDesc.type = BufferDesc::Type::Index;
            tile.gpuIndexBuffer = device_->createBuffer(ibDesc);
        }
    }

    tile.meshReady = true;
}

void Tileset::buildTileDrawCommand(Renderer& renderer, TilesetTile& tile,
                                    RenderCommandList& commands) {
    if (!tile.meshReady) {
        ensureTileMesh(tile);
    }
    if (!tile.meshReady || !tile.gpuVertexBuffer) return;

    // Update raster overlays
    Texture* tex = nullptr;
    float uvOffU = 0, uvOffV = 0, uvScaleU = 1, uvScaleV = 1;

    for (size_t i = 0; i < imageryLayers_.size() && i < tile.rasterOverlays.size(); ++i) {
        auto& overlay = tile.rasterOverlays[i];
        if (!overlay) {
            overlay = std::make_unique<RasterMappedToTilesetTile>();
        }
        overlay->update(tile.key, tile.bounds, imageryLayers_[i], device_);
        if (overlay->state() != RasterMappedToTilesetTile::State::Unattached) {
            tex = overlay->texture();
            uvOffU = overlay->offsetU();
            uvOffV = overlay->offsetV();
            uvScaleU = overlay->scaleU();
            uvScaleV = overlay->scaleV();
            break;  // Use first available overlay
        }
    }

    // Placeholder texture if no imagery
    if (!tex) {
        tex = imageryLayers_.empty() ? nullptr :
              imageryLayers_[0]->placeholderTexture();
    }

    auto cmd = renderer.makeSurfaceTileCommand(
        tex,
        tile.gpuVertexBuffer.get(),
        tile.gpuIndexBuffer.get(),
        static_cast<int>(tile.mesh->indices.size()));
    cmd.surfaceTileUv = {uvOffU, uvOffV, uvScaleU, uvScaleV};
    cmd.surfaceTileOrigin = {
        static_cast<float>(tile.localOrigin.x()),
        static_cast<float>(tile.localOrigin.y()),
        static_cast<float>(tile.localOrigin.z())
    };
    cmd.surfaceTileOpacity = 1.0f;
    cmd.surfaceTransitionOpacity = 1.0f;
    cmd.surfaceGeneration = static_cast<float>(generation_);
    commands.push_back(std::move(cmd));
}

void Tileset::buildRenderCommands(Renderer& renderer,
                                   RenderCommandList& commands) {
    ++frameNumber_;

    // Build tiles from visible keys
    for (const TileKey& key : tilePlan_.visibleTiles) {
        std::string ck = terrainCacheKey(key);
        auto& tile = tiles_[ck];
        if (!tile) {
            tile = std::make_unique<TilesetTile>(key, tileScheme_->tileToRectangle(key));
            constexpr double kMaxGE = 6378137.0 * 0.25 / 65.0;
            tile->geometricError = 8.0 * kMaxGE * tile->bounds.width();
            tile->rasterOverlays.resize(imageryLayers_.size());
        }
        tile->lastUsedFrame = frameNumber_;
        buildTileDrawCommand(renderer, *tile, commands);
    }

    // cesium-native LRU eviction: purge tiles not used recently
    evictUnusedTiles();
}

void Tileset::evictUnusedTiles() {
    if (tiles_.size() <= kMaxCachedTiles) return;

    // Collect tiles by last used frame, sort oldest first
    std::vector<std::pair<std::string, uint64_t>> entries;
    entries.reserve(tiles_.size());
    for (const auto& [key, tile] : tiles_) {
        entries.emplace_back(key, tile->lastUsedFrame);
    }
    std::sort(entries.begin(), entries.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });

    // Evict oldest tiles down to kMaxCachedTiles
    size_t toRemove = tiles_.size() - kMaxCachedTiles;
    for (size_t i = 0; i < toRemove && i < entries.size(); ++i) {
        tiles_.erase(entries[i].first);
    }
}

} // namespace earth_engine
