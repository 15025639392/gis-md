#include "BasemapLayer.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../renderer/Renderer.h"
#include "../renderer/RenderDevice.h"
#include "../core/math/Rectangle.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstring>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace earth_engine {

BasemapLayer::BasemapLayer(std::unique_ptr<ImageryProvider> provider,
                            std::unique_ptr<TileScheme> tileScheme,
                            RenderDevice* renderDevice)
    : id_(provider->id()),
      provider_(std::move(provider)),
      tileScheme_(std::move(tileScheme)),
      renderDevice_(renderDevice),
      textureCache_(renderDevice, 64 * 1024 * 1024),
      pendingQueue_(std::make_shared<PendingQueue>()) {}

BasemapLayer::~BasemapLayer() = default;

void BasemapLayer::update(const FrameState& frameState) {
    if (!visible_ || !frameState.camera) return;

    // 1. 处理后台线程完成的解码，上传 GPU 纹理（主线程安全）
    processPendingUploads();

    // 2. 计算可见瓦片
    tilePlan_ = TilePlanBuilder::compute(
        *frameState.camera, *tileScheme_,
        static_cast<double>(frameState.viewportWidthPixels),
        static_cast<double>(frameState.viewportHeightPixels),
        tilePlan_.zoom);

    // 3. 请求缺失的瓦片
    loadMissingTiles();
}

void BasemapLayer::applyPlan(const TilePlan& plan) {
    if (!visible_) return;

    processPendingUploads();
    if (plan.zoom != tilePlan_.zoom || plan.visibleTiles != tilePlan_.visibleTiles) {
        ++generation_;
    }
    tilePlan_ = plan;
}

void BasemapLayer::loadMissingTiles() {
    constexpr double kRetryBackoffSec = 2.0;
    constexpr int kMaxRetries = 3;

    auto now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // 清理过期失败记录
    for (auto it = failedTiles_.begin(); it != failedTiles_.end(); ) {
        if (now - it->second.firstFailTime > 30.0) {
            it = failedTiles_.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& key : tilePlan_.visibleTiles) {
        if (textureCache_.contains(key)) continue;

        std::string ck = std::to_string(key.z) + "/" +
                         std::to_string(key.x) + "/" +
                         std::to_string(key.y);

        auto it = failedTiles_.find(ck);
        if (it != failedTiles_.end()) {
            if (it->second.retries >= kMaxRetries) continue;
            if (now - it->second.firstFailTime < kRetryBackoffSec) continue;
        }
        auto requested = requestedGeneration_.find(ck);
        if (requested != requestedGeneration_.end() && requested->second == generation_) {
            continue;
        }

        loadTile(key);
    }
}

void BasemapLayer::loadTile(const TileKey& key) {
    auto queue = pendingQueue_;  // shared_ptr copy protects against ~BasemapLayer
    auto token = CancellationToken();
    const uint64_t generation = generation_;

    std::string ck = std::to_string(key.z) + "/" +
                     std::to_string(key.x) + "/" +
                     std::to_string(key.y);
    requestedGeneration_[ck] = generation;

    provider_->requestTile(key, token,
        [queue, key, generation](const TileKey& k, std::unique_ptr<DecodedImage> image) {
            if (!image) return;  // 失败由 loadMissingTiles 超时重试处理
            std::lock_guard<std::mutex> lock(queue->mutex);
            queue->queue.push_back({k, generation, std::move(image)});
        });

    // 记录请求时间，用于失败检测
    auto now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto& ft = failedTiles_[ck];
    if (ft.firstFailTime == 0.0) ft.firstFailTime = now;
    ft.retries++;
}

void BasemapLayer::processPendingUploads() {
    std::deque<PendingUpload> batch;
    {
        std::lock_guard<std::mutex> lock(pendingQueue_->mutex);
        batch.swap(pendingQueue_->queue);
    }

    for (auto& item : batch) {
        if (item.generation != generation_) {
            continue;
        }
        auto& image = item.image;
        TextureDesc texDesc;
        texDesc.width = image->width;
        texDesc.height = image->height;
        texDesc.format = (image->channels == 4)
                             ? TextureDesc::Format::RGBA8
                             : TextureDesc::Format::RGB8;
        texDesc.data = image->pixels.data();
        texDesc.dataSize = image->pixels.size();
        texDesc.mipmap = true;
        texDesc.minFilter = TextureDesc::Filter::Linear;
        texDesc.wrapS = TextureDesc::Wrap::Clamp;
        texDesc.wrapT = TextureDesc::Wrap::Clamp;

        auto texture = renderDevice_->createTexture(texDesc);
        if (texture) {
            // 成功上传 → 清除失败记录
            std::string ck = std::to_string(item.key.z) + "/" +
                             std::to_string(item.key.x) + "/" +
                             std::to_string(item.key.y);
            failedTiles_.erase(ck);
            requestedGeneration_.erase(ck);
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_INFO, "BasemapLayer",
                "Tile uploaded: %d/%d/%d %dx%d",
                item.key.z, item.key.x, item.key.y,
                image->width, image->height);
#endif
            textureCache_.put(item.key, std::move(texture));
        }
    }
}

void BasemapLayer::buildRenderCommands(Renderer& renderer,
                                        RenderCommandList& commands) {
    if (!visible_) return;

    for (const auto& key : tilePlan_.visibleTiles) {
        Texture* tex = textureCache_.get(key);
        TileKey textureKey = key;

        // 父瓦片回退：如果子级未缓存，尝试上一级 zoom 的父级
        if (!tex) {
            for (const auto& parentKey : tilePlan_.parentTiles) {
                tex = textureCache_.get(parentKey);
                if (tex) {
                    textureKey = parentKey;
                    break;
                }
            }
        }

        if (!tex) continue;

        Rectangle bounds = tileScheme_->tileToRectangle(key);
        Rectangle textureBounds = tileScheme_->tileToRectangle(textureKey);
        const double textureWidth = textureBounds.east() - textureBounds.west();
        const double textureHeight = textureBounds.north() - textureBounds.south();
        float uvOffsetX = 0.0f;
        float uvOffsetY = 0.0f;
        float uvScaleX = 1.0f;
        float uvScaleY = 1.0f;
        if (textureKey != key && textureWidth != 0.0 && textureHeight != 0.0) {
            uvOffsetX = static_cast<float>((bounds.west() - textureBounds.west()) / textureWidth);
            uvScaleX = static_cast<float>((bounds.east() - bounds.west()) / textureWidth);
            uvOffsetY = static_cast<float>((textureBounds.north() - bounds.north()) / textureHeight);
            uvScaleY = static_cast<float>((bounds.north() - bounds.south()) / textureHeight);
        }

        auto cmd = renderer.makeTileCommand(
            tex,
            static_cast<float>(bounds.west()),
            static_cast<float>(bounds.south()),
            static_cast<float>(bounds.east()),
            static_cast<float>(bounds.north()),
            uvOffsetX,
            uvOffsetY,
            uvScaleX,
            uvScaleY);

        commands.push_back(std::move(cmd));
    }
}

} // namespace earth_engine
