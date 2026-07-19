#include "TerrainPageStore.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <vector>

#include "../debug/PlatformLog.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../platform/bridge/PlatformBridge.h"  // DecodedImage
#include "../providers/ImageryProvider.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "../tiling/TileScheme.h"
#include "../tiling/TilesetTile.h"
#include "RenderCommand.h"
#include "RenderDevice.h"

namespace earth_engine {

namespace {

// 影像行序:与生产 raster uploader 一致——DecodedImage row 0=北,行序原样上传,
// terrain 采用 NW 约定(v=0 北,rewriteProjectionTexCoords)故不翻转即对齐
// (RenderDeviceRasterTextureUploader 也是 createTexture 原样上传无翻转)。
// 真机若上下颠倒则翻此常量。
constexpr bool kFlipRowsOnUpload = false;

// Step 3b 影像未到达前的占位色(逐层可区分,渐次被真实影像覆盖)。
constexpr std::array<std::array<uint8_t, 4>, 4> kPlaceholderColors = {{
    {90, 90, 96, 255},
    {96, 90, 90, 255},
    {90, 96, 90, 255},
    {90, 90, 96, 255},
}};

}  // namespace

// worker 回调把解码影像投进本箱(shared_ptr 持有 → 即使 TerrainPageStore 已析构
// 也不悬垂);渲染线程 drainInbox 取走上传。
struct TerrainPageStore::PendingInbox {
    std::mutex mutex;
    std::vector<std::pair<int, std::unique_ptr<DecodedImage>>> pages;
};

TerrainPageStore::~TerrainPageStore() {
    fetchToken_.cancel();  // 尽力取消在途 fetch(回调仍安全:只写 shared inbox)
}

bool TerrainPageStore::initialize(RenderDevice* device, const Config& config) {
    if (!device) {
        return false;
    }
    device_ = device;
    config_ = config;
    config_.pageSizeTexels = std::max(1, config_.pageSizeTexels);
    config_.depthLevels = std::clamp(config_.depthLevels, 0, 4);
    config_.gridN = 1 << config_.depthLevels;  // gridN 恒 = 2^depthLevels
    const int layers = config_.gridN * config_.gridN;

    // texture2DArray:pageSize×pageSize×layers,RGBA8,逐层 CLAMP_TO_EDGE(§13.1
    // 无页缝),linear(真实影像放大平滑)。
    TextureDesc desc;
    desc.width = config_.pageSizeTexels;
    desc.height = config_.pageSizeTexels;
    desc.arrayLayers = layers;
    desc.format = TextureDesc::Format::RGBA8;
    desc.mipmap = false;
    desc.minFilter = TextureDesc::Filter::Linear;
    desc.magFilter = TextureDesc::Filter::Linear;
    desc.wrapS = TextureDesc::Wrap::Clamp;
    desc.wrapT = TextureDesc::Wrap::Clamp;
    arrayTexture_ = device_->createTexture(desc);
    if (!arrayTexture_) {
        device_ = nullptr;
        return false;
    }
    inbox_ = std::make_shared<PendingInbox>();
    fillSyntheticPlaceholder();
    return true;
}

void TerrainPageStore::fillSyntheticPlaceholder() {
    const int side = config_.pageSizeTexels;
    const int layers = config_.gridN * config_.gridN;
    std::vector<uint8_t> page(static_cast<size_t>(side) *
                              static_cast<size_t>(side) * 4u);
    for (int layer = 0; layer < layers; ++layer) {
        const std::array<uint8_t, 4>& c =
            kPlaceholderColors[static_cast<size_t>(layer) %
                               kPlaceholderColors.size()];
        for (size_t px = 0; px < page.size(); px += 4) {
            page[px + 0] = c[0];
            page[px + 1] = c[1];
            page[px + 2] = c[2];
            page[px + 3] = c[3];
        }
        device_->updateTextureRegion(arrayTexture_.get(), 0, 0, side, side,
                                     page.data(),
                                     static_cast<size_t>(side) * 4u, layer);
    }
}

void TerrainPageStore::applyToTerrainCommand(
    RenderCommand& cmd, const TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& overlays) {
    if (!arrayTexture_ || !cmd.terrainRenderContent) {
        return;
    }
    // 只挂真实地形(fill/ellipsoid proxy 不是最终高清目标)。
    if (cmd.terrainSurfaceSource != TerrainSurfaceCommandSource::RealTerrain) {
        return;
    }
    // 目标 = 屏幕空间误差最大(最近/最占屏)的真实地形瓦片,保证可见可辨。
    const double sse = tile.selectionFrameState.screenSpaceError;
    if (!targetLocked_ || sse > bestSse_) {
        targetKey_ = tile.key;
        bestSse_ = sse;
        targetLocked_ = true;
        targetBounds_ = tile.bounds;
        targetZ_ = tile.key.z;
        // 捕获影像 provider(拉真实高清影像);目标变了重新 fetch。
        provider_ = overlays.empty() ? nullptr
                                     : overlays.front()->getTileProvider();
        fetchKicked_ = false;
        platformLog(LogLevel::Warning, "PageStore",
                    "target -> z%d/%d/%d sse=%.1f provider=%d", tile.key.z,
                    tile.key.x, tile.key.y, sse, provider_ ? 1 : 0);
    }
    if (tile.key != targetKey_) {
        return;
    }

    if (cmd.textures.size() <=
        static_cast<size_t>(kGltfPageStoreArrayTextureSlot)) {
        cmd.textures.resize(
            static_cast<size_t>(kGltfPageStoreArrayTextureSlot) + 1u, nullptr);
    }
    cmd.textures[kGltfPageStoreArrayTextureSlot] = arrayTexture_.get();
    // enabled=1、gridN、layerBase=0 → 片元改采页存储,覆盖上采样 mappedRaster。
    cmd.gltfUniforms.pageStoreParams = {
        1.0f, static_cast<float>(config_.gridN), 0.0f, 0.0f};
}

void TerrainPageStore::tick() {
    if (!arrayTexture_) {
        return;
    }
    if (targetLocked_ && !fetchKicked_ && provider_) {
        kickImageryFetch();
        fetchKicked_ = true;
    }
    drainInbox();
}

void TerrainPageStore::kickImageryFetch() {
    ImageryProvider& imagery = provider_->getImageryProvider();
    const TileScheme& scheme = provider_->getTileScheme();
    const int gridN = config_.gridN;
    const int zoom = targetZ_ + config_.depthLevels;
    if (zoom > provider_->getMaximumLevel()) {
        // 影像深度不足以铺 gridN×gridN 对齐子瓦片:跳过 fetch,留占位(不做
        // 部分覆盖以免错位)。原型阶段影像 maxZoom≫z12+2,实际不触发。
        platformLog(LogLevel::Warning, "PageStore",
                    "skip fetch: zoom %d > maxLevel %d", zoom,
                    provider_->getMaximumLevel());
        return;
    }
    // **mercator 直接子瓦片**(修 lat-linear 枚举错位):terrain 与影像同 XYZ
    // web-mercator 分块,故目标瓦片 (z,x,y) 的 z+depth 子瓦片直接 =
    // (x*gridN+dx, y*gridN+dy),与 shader mercator UV 的 cell 网格逐格对齐
    // (NW 约定 v=0 北 → cell.y=0=北 = 最小 y = y*gridN+0)。影像 schemeId 经
    // positionToTile(瓦片中心) 取(interned);顺带校验分块与 terrain 一致。
    const double centerLng =
        0.5 * (targetBounds_.west() + targetBounds_.east());
    const double centerLat =
        0.5 * (targetBounds_.south() + targetBounds_.north());
    const TileKey centerImg =
        scheme.positionToTile(centerLng, centerLat, targetZ_);
    const bool aligned = centerImg.z == targetKey_.z &&
                         centerImg.x == targetKey_.x &&
                         centerImg.y == targetKey_.y;
    platformLog(LogLevel::Warning, "PageStore",
                "kick fetch: %d tiles @ z%d (gridN=%d) aligned=%d img(%d/%d/%d)",
                gridN * gridN, zoom, gridN, aligned ? 1 : 0, centerImg.z,
                centerImg.x, centerImg.y);
    for (int dy = 0; dy < gridN; ++dy) {
        for (int dx = 0; dx < gridN; ++dx) {
            TileKey childKey;
            childKey.schemeId = centerImg.schemeId;
            childKey.z = zoom;
            childKey.x = targetKey_.x * gridN + dx;
            childKey.y = targetKey_.y * gridN + dy;
            const int layer = dy * gridN + dx;  // 与 shader cell.y*gridN+cell.x 对齐
            std::shared_ptr<PendingInbox> inbox = inbox_;
            imagery.requestTile(
                childKey, fetchToken_,
                [inbox, layer](const TileKey&,
                               std::unique_ptr<DecodedImage> image) {
                    if (!image) return;
                    std::lock_guard<std::mutex> lock(inbox->mutex);
                    inbox->pages.emplace_back(layer, std::move(image));
                });
        }
    }
}

void TerrainPageStore::drainInbox() {
    std::vector<std::pair<int, std::unique_ptr<DecodedImage>>> ready;
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        ready.swap(inbox_->pages);
    }
    if (ready.empty()) {
        return;
    }
    const int side = config_.pageSizeTexels;
    std::vector<uint8_t> rgba(static_cast<size_t>(side) *
                              static_cast<size_t>(side) * 4u);
    for (auto& [layer, image] : ready) {
        if (!image || image->width != side || image->height != side ||
            image->pixels.empty()) {
            continue;  // 尺寸不符(非 256²)跳过,保留占位
        }
        const int ch = image->channels;
        const uint8_t* src = image->pixels.data();
        for (int row = 0; row < side; ++row) {
            const int srcRow = kFlipRowsOnUpload ? (side - 1 - row) : row;
            const uint8_t* s = src + static_cast<size_t>(srcRow) * side * ch;
            uint8_t* d = rgba.data() + static_cast<size_t>(row) * side * 4;
            for (int col = 0; col < side; ++col) {
                if (ch >= 3) {
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                    d[3] = ch >= 4 ? s[3] : 255;
                } else {  // 单通道:灰度铺三通道
                    d[0] = d[1] = d[2] = s[0];
                    d[3] = 255;
                }
                s += ch;
                d += 4;
            }
        }
        device_->updateTextureRegion(arrayTexture_.get(), 0, 0, side, side,
                                     rgba.data(),
                                     static_cast<size_t>(side) * 4u, layer);
        ++uploadedLayers_;
    }
    platformLog(LogLevel::Warning, "PageStore", "real imagery layers %d/%d",
                uploadedLayers_, config_.gridN * config_.gridN);
}

}  // namespace earth_engine
