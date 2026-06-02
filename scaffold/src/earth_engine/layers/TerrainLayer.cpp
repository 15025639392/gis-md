#include "TerrainLayer.h"
#include "../terrain/TerrainMesh.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../renderer/Renderer.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Cartographic.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstring>
#include <algorithm>

namespace earth_engine {

TerrainLayer::TerrainLayer(std::unique_ptr<TerrainProvider> provider,
                            std::unique_ptr<TileScheme> tileScheme)
    : id_(provider->id()),
      provider_(std::move(provider)),
      tileScheme_(std::move(tileScheme)),
      pendingQueue_(std::make_shared<PendingQueue>()) {}

TerrainLayer::~TerrainLayer() = default;

// ============================================================
// 高度采样
// ============================================================

float TerrainLayer::sampleHeight(double lngRad, double latRad) const {
    const TerrainTile* best = findBestTile(lngRad, latRad);
    if (!best) return 0.0f;
    return best->sampleHeight(lngRad, latRad);
}

const TerrainTile* TerrainLayer::findBestTile(double lngRad, double latRad) const {
    // 简单查找：遍历缓存，找到覆盖该坐标且 zoom 最高的 tile
    const TerrainTile* best = nullptr;
    int bestZoom = -1;
    for (const auto& [key, tile] : tileCache_) {
        if (!tile->valid()) continue;
        if (tile->bounds().contains(lngRad, latRad)) {
            if (tile->key().z > bestZoom) {
                bestZoom = tile->key().z;
                best = tile.get();
            }
        }
    }
    return best;
}

// ============================================================
// 帧更新
// ============================================================

void TerrainLayer::update(const FrameState& frameState) {
    if (!enabled_ || !visible_ || !frameState.camera) return;

    // 1. 处理后台线程完成的解码
    processPendingUploads();

    // 2. 计算可见瓦片
    tilePlan_ = TilePlanBuilder::compute(
        *frameState.camera, *tileScheme_,
        static_cast<double>(frameState.viewportWidthPixels),
        static_cast<double>(frameState.viewportHeightPixels));

    // 3. 请求缺失的瓦片（限制 zoom 范围到 provider 支持的范围）
    int requestsThisUpdate = 0;
    constexpr int kMaxTerrainRequestsPerUpdate = 8;
    for (const auto& key : tilePlan_.visibleTiles) {
        if (key.z < provider_->minZoom() || key.z > provider_->maxZoom()) continue;
        std::string cacheKey = std::to_string(key.z) + "/" +
                               std::to_string(key.x) + "/" +
                               std::to_string(key.y);
        if (tileCache_.find(cacheKey) == tileCache_.end() &&
            requestedTiles_.find(cacheKey) == requestedTiles_.end()) {
            if (requestsThisUpdate >= kMaxTerrainRequestsPerUpdate) break;
            requestedTiles_.insert(cacheKey);
            loadTile(key);
            ++requestsThisUpdate;
        }
    }
}

void TerrainLayer::loadTile(const TileKey& key) {
    auto queue = pendingQueue_;  // shared_ptr copy protects against ~TerrainLayer
    CancellationToken token;

    provider_->requestTile(key, token,
        [queue, key](const TileKey& k, std::unique_ptr<DecodedHeightmap> hm) {
            if (!hm) return;
            std::lock_guard<std::mutex> lock(queue->mutex);
            queue->queue.push_back({k, std::move(hm)});
        });
}

void TerrainLayer::processPendingUploads() {
    std::deque<PendingUpload> batch;
    {
        std::lock_guard<std::mutex> lock(pendingQueue_->mutex);
        batch.swap(pendingQueue_->queue);
    }

    for (auto& item : batch) {
        std::string cacheKey = std::to_string(item.key.z) + "/" +
                               std::to_string(item.key.x) + "/" +
                               std::to_string(item.key.y);

        auto tile = std::make_unique<TerrainTile>(
            item.key, *tileScheme_, std::move(item.heightmap));

        tileCache_[cacheKey] = std::move(tile);
        meshDirty_ = true;
    }
}

// ============================================================
// 渲染命令
// ============================================================

void TerrainLayer::buildRenderCommands(const GlobeMesh& baseGlobeMesh,
                                        const FrameState& frameState,
                                        Renderer& renderer,
                                        RenderCommandList& commands) {
    if (!enabled_ || !visible_) return;
    if (!frameState.camera) return;

    // 查找最佳覆盖瓦片（最高 zoom 且覆盖屏幕中心）
    // 简化：取第一个有效的高 zoom tile
    const TerrainTile* bestTile = nullptr;
    int bestZoom = -1;
    for (const auto& [key, tile] : tileCache_) {
        if (tile->valid() && tile->key().z > bestZoom) {
            bestZoom = tile->key().z;
            bestTile = tile.get();
        }
    }

    // 生成地形网格（含裙边防止 tile 间裂缝）并上传到 GPU
    if ((meshDirty_ || bestTile) && bestTile) {
        cachedMesh_ = TerrainMeshBuilder::build(baseGlobeMesh, bestTile, -50.0f);
        renderer.updateGlobeMesh(cachedMesh_);
        meshDirty_ = false;
    }

    // 为地形网格生成 RenderCommand（使用与 Globe 相同的 shader）
    RenderCommand cmd;
    cmd.owner = "terrain";
    cmd.pass = "color";
    cmd.shader = renderer.globeShader();
    cmd.primitive = RenderCommand::PrimitiveType::Triangles;
    cmd.indexType = RenderCommand::IndexType::UInt32;
    cmd.depthTest = true;
    cmd.depthWrite = true;

    // 使用 Renderer 的 globe vertex/index buffer（已通过 updateGlobeMesh 上传位移+裙边）
    cmd.vertexBuffer = renderer.globeVertexBuffer();
    cmd.indexBuffer = renderer.globeIndexBuffer();
    cmd.indexCount = renderer.globeIndexCount();

    // 设置 MVP
    if (frameState.camera) {
        const Camera& cam = *frameState.camera;
        float vpW = static_cast<float>(frameState.viewportWidthPixels);
        float vpH = static_cast<float>(frameState.viewportHeightPixels);

        glm::mat4 model = glm::make_mat4(Renderer::earthModelMatrix().data());
        glm::mat4 view(cam.viewMatrix().raw());
        glm::mat4 proj(cam.projectionMatrix(vpW, vpH).raw());
        glm::mat4 mvp = proj * view * model;

        auto& mvpU = cmd.uniforms["u_modelViewProjection"];
        mvpU.resize(16);
        std::memcpy(mvpU.data(), glm::value_ptr(mvp), 16 * sizeof(float));

        auto& modelU = cmd.uniforms["u_model"];
        modelU.resize(16);
        std::memcpy(modelU.data(), glm::value_ptr(model), 16 * sizeof(float));
    }

    cmd.uniforms["u_lightDir"] = {
        frameState.lightDir.x,
        frameState.lightDir.y,
        frameState.lightDir.z
    };

    commands.push_back(std::move(cmd));
}

} // namespace earth_engine
