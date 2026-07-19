#include "TerrainPageStore.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

#include "../core/math/OrientedBoundingBox.h"
#include "../debug/PlatformLog.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../platform/bridge/PlatformBridge.h"  // DecodedImage
#include "../providers/ImageryProvider.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "../scene/Frustum.h"
#include "../scene/SelectorView.h"
#include "../tiling/GltfDrawCommandBuilder.h"  // terrainSurfaceSourceForDraw
#include "../tiling/TileBoundsMetrics.h"
#include "../tiling/TileScheme.h"
#include "../tiling/TilesetTile.h"
#include "RenderCommand.h"
#include "RenderDevice.h"

namespace earth_engine {

// ============================================================
// TerrainPageLayerPool — 等尺寸块 LRU 分配器(纯 CPU)
// ============================================================

void TerrainPageLayerPool::configure(int blockCount, int blockLayers) {
    blockLayers_ = std::max(1, blockLayers);
    slots_.assign(static_cast<size_t>(std::max(0, blockCount)), Slot{});
    keyToSlot_.clear();
}

int TerrainPageLayerPool::layerBaseFor(uint64_t key) const {
    const auto it = keyToSlot_.find(key);
    if (it == keyToSlot_.end()) {
        return -1;
    }
    return it->second * blockLayers_;
}

int TerrainPageLayerPool::acquire(uint64_t key, uint64_t frameId,
                                  uint64_t* outEvicted) {
    if (outEvicted) {
        *outEvicted = 0;
    }
    // 已驻留:touch + 返回。
    if (const auto it = keyToSlot_.find(key); it != keyToSlot_.end()) {
        slots_[static_cast<size_t>(it->second)].lastFrame = frameId;
        return it->second * blockLayers_;
    }
    // 找空块。
    int slot = -1;
    for (size_t i = 0; i < slots_.size(); ++i) {
        if (!slots_[i].used) {
            slot = static_cast<int>(i);
            break;
        }
    }
    // 无空块:淘汰 lastFrame < frameId 的最久块(本帧已 touch 的块不动)。
    if (slot < 0) {
        uint64_t best = std::numeric_limits<uint64_t>::max();
        for (size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].lastFrame < frameId && slots_[i].lastFrame < best) {
                best = slots_[i].lastFrame;
                slot = static_cast<int>(i);
            }
        }
        if (slot < 0) {
            return -1;  // 全部本帧可见 → 回落 mappedRaster
        }
        if (outEvicted) {
            *outEvicted = slots_[static_cast<size_t>(slot)].key;
        }
        keyToSlot_.erase(slots_[static_cast<size_t>(slot)].key);
    }
    slots_[static_cast<size_t>(slot)] = Slot{true, key, frameId};
    keyToSlot_[key] = slot;
    return slot * blockLayers_;
}

void TerrainPageLayerPool::release(uint64_t key) {
    const auto it = keyToSlot_.find(key);
    if (it == keyToSlot_.end()) {
        return;
    }
    slots_[static_cast<size_t>(it->second)] = Slot{};
    keyToSlot_.erase(it);
}

// ============================================================
// TerrainPageStore — 多瓦片页表
// ============================================================

namespace {

// 影像行序:与生产 raster uploader 一致——DecodedImage row 0=北,行序原样上传,
// terrain 采用 NW 约定(v=0 北,rewriteProjectionTexCoords)故不翻转即对齐。
constexpr bool kFlipRowsOnUpload = false;

}  // namespace

// worker 回调把解码影像投进本箱(shared_ptr 持有 → 即使 TerrainPageStore 已析构
// 也不悬垂);渲染线程 drainInbox 取走上传。每项带 packed key + 绝对 layer,
// drain 时校验 entry 仍驻留于该 layerBase(淘汰后到达的丢弃)。
struct TerrainPageStore::PendingInbox {
    struct Item {
        uint64_t key = 0;
        int layer = 0;  // 绝对层索引(layerBase + offset)
        std::unique_ptr<DecodedImage> image;
    };
    std::mutex mutex;
    std::vector<Item> pages;
};

void TerrainPageStore::encodeLayerRGBA8(int layer, uint8_t out[4]) {
    const unsigned int v = static_cast<unsigned int>(std::max(0, layer));
    out[0] = static_cast<uint8_t>(v & 0xFFu);          // R = layer & 0xFF
    out[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);   // G = (layer >> 8) & 0xFF
    out[2] = 0;                                         // B 保留
    out[3] = 255;                                       // A 保留(不透明)
}

int TerrainPageStore::decodeLayerRGBA8(const uint8_t in[4]) {
    // 逐位镜像片元 shader:floor(r*255+0.5)+floor(g*255+0.5)*256(RGBA8 unorm
    // 采样后 r=R/255,floor(R+0.5)=R)。
    return static_cast<int>(in[0]) + static_cast<int>(in[1]) * 256;
}

std::unique_ptr<Texture> TerrainPageStore::buildIndirTexture(int layerBase,
                                                             int gridN) {
    // dense 填:texel[cy][cx] = encode(layerBase + cy*gridN + cx),与闭式公式
    // cell.y*gridN+cell.x 逐 cell 等价(NW 无翻转:cy=0=北=row0=最小 v,和影像
    // 上传 kFlipRowsOnUpload=false + shader cell.y 一致)。片元 (cell+0.5)/gridN
    // 命中 texel 中心 → NEAREST 取精确 layer。
    std::vector<uint8_t> texels(static_cast<size_t>(gridN) *
                                static_cast<size_t>(gridN) * 4u);
    for (int cy = 0; cy < gridN; ++cy) {
        for (int cx = 0; cx < gridN; ++cx) {
            const int layer = layerBase + cy * gridN + cx;
            encodeLayerRGBA8(layer,
                             texels.data() +
                                 (static_cast<size_t>(cy) * gridN + cx) * 4u);
        }
    }
    TextureDesc desc;
    desc.width = gridN;
    desc.height = gridN;
    desc.arrayLayers = 1;  // 普通 2D(非 array):间接纹理是索引表,非页存储
    desc.format = TextureDesc::Format::RGBA8;
    desc.data = texels.data();
    desc.dataSize = texels.size();
    desc.mipmap = false;
    // NEAREST:间接纹理必须点采样取精确 texel(线性会在 cell 边界串值→错 layer)。
    desc.minFilter = TextureDesc::Filter::Nearest;
    desc.magFilter = TextureDesc::Filter::Nearest;
    desc.wrapS = TextureDesc::Wrap::Clamp;
    desc.wrapT = TextureDesc::Wrap::Clamp;
    return device_->createTexture(desc);
}

uint64_t TerrainPageStore::packKey(const TileKey& key) {
    // capped 瓦片 z 小(≤~14),x/y < 2^z < 2^22,可无损打包进 64 位。
    return (static_cast<uint64_t>(key.z) << 44) |
           (static_cast<uint64_t>(key.x) << 22) |
           static_cast<uint64_t>(key.y);
}

// ============================================================
// 门② 屏幕可见影像页 determination(Step B2a,纯读 + 插桩)
// ============================================================

int TerrainPageStore::subtileGridN(int tileZ, int sourceZoom) {
    if (sourceZoom <= tileZ) {
        return 1;  // 影像不比瓦片深 → 不细分(1×1,即瓦片自身)。
    }
    return 1 << (sourceZoom - tileZ);
}

void TerrainPageStore::enumerateSubtileKeys(const TileKey& tileKey,
                                            int sourceZoom,
                                            std::vector<TileKey>& out) {
    out.clear();
    const int gridN = subtileGridN(tileKey.z, sourceZoom);
    out.reserve(static_cast<size_t>(gridN) * static_cast<size_t>(gridN));
    // mercator 直接子瓦片(§13.4):(z+depth) 子瓦片 = (x*gridN+dx, y*gridN+dy),
    // 与 shader mercator cell 网格逐格对齐(NW:dy=0=北=最小 y)。勿用 lat 均分。
    for (int dy = 0; dy < gridN; ++dy) {
        for (int dx = 0; dx < gridN; ++dx) {
            TileKey child;
            child.schemeId = tileKey.schemeId;  // 同 XYZ 分块 → 沿用 schemeId
            child.z = sourceZoom;
            child.x = tileKey.x * gridN + dx;
            child.y = tileKey.y * gridN + dy;
            out.push_back(child);
        }
    }
}

void TerrainPageStore::updateVisiblePages(
    const SelectorView& view,
    const std::vector<TilesetTile*>& visibleTiles,
    RasterOverlayTileProvider* provider,
    double terrainMaxScreenSpaceError) {
    ++pageDetFrameCounter_;
    if (!provider || visibleTiles.empty()) {
        return;
    }
    const TileScheme& scheme = provider->getTileScheme();
    const int providerMaxLevel = provider->getMaximumLevel();

    visiblePagesScratch_.clear();
    int visibleCappedTiles = 0;
    int zMin = std::numeric_limits<int>::max();
    int zMax = std::numeric_limits<int>::min();
    double maxTileSse = 0.0;  // TEMP 诊断:本帧最大瓦片屏幕 SSE(驱动 zoom)

    for (const TilesetTile* tile : visibleTiles) {
        if (!tile) {
            continue;
        }
        // 只算真实地形瓦片(与 draw 命令构建同一判定,单一事实源)。
        if (terrainSurfaceSourceForDraw(tile->content.renderContent) !=
            TerrainSurfaceCommandSource::RealTerrain) {
            continue;
        }
        ++visibleCappedTiles;

        // 1) 屏幕合适的影像源 zoom = **相机相关**(关键:不能用瓦片几何误差,那是
        //    静态的、会复现几何/影像耦合 → capped z12 瓦片得 z12)。瓦片几何在屏幕
        //    上占 tileSSE 像素误差(selectionFrameState.screenSpaceError,距离已烘进去);
        //    若不 cap,selector 会把几何细化到 SSE≤地形阈值 = 深 log2(tileSSE/阈值) 级。
        //    影像匹配那个「本应细化到的几何 LOD」= 耦合态清晰度(#3 近景 z16-17)。
        //    **分母用地形细化阈值(16),不是 overlay MSE(2)**——tileSSE 就是对着 16
        //    阈值量的;用 2 会深 log2(8)=3 级 → z18 过取。
        const double tileSse = tile->selectionFrameState.screenSpaceError;
        maxTileSse = std::max(maxTileSse, tileSse);
        const double threshold = std::max(1e-3, terrainMaxScreenSpaceError);
        int zoom = tile->key.z;
        if (tileSse > threshold) {
            zoom = tile->key.z +
                   static_cast<int>(std::lround(std::log2(tileSse / threshold)));
        }
        // 2) clamp:≥ 瓦片自身 z(否则不细分),≤ provider maxLevel,且深度 cap
        //    (防远景/病态 zoom 枚举爆量)。
        zoom = std::min(zoom, providerMaxLevel);
        zoom = std::min(zoom, tile->key.z + kMaxDetDepthLevels);
        zoom = std::max(zoom, tile->key.z);

        // 3) 枚举子瓦片 + 视锥剔除。minH/maxH 取瓦片自身高度范围(缺省 -1000/9000)。
        enumerateSubtileKeys(tile->key, zoom, subtileScratch_);
        const double minH = TileBoundsMetrics::terrainMinimumHeight(*tile);
        const double maxH = TileBoundsMetrics::terrainMaximumHeight(*tile);
        for (const TileKey& sub : subtileScratch_) {
            const Rectangle subRect = scheme.tileToRectangle(sub);
            const std::optional<OrientedBoundingBox> obb =
                TileBoundsMetrics::boundingRegionObb(subRect, minH, maxH);
            if (!obb || !view.frustum.intersectsOBB(*obb)) {
                continue;  // 视锥外 → 不是可见页
            }
            // 4) 汇入唯一页集合(跨瓦片可能共享祖先 → 去重)。
            visiblePagesScratch_.insert(packKey(sub));
            zMin = std::min(zMin, sub.z);
            zMax = std::max(zMax, sub.z);
        }
    }

    lastVisiblePageCount_ = static_cast<int>(visiblePagesScratch_.size());
    // 插桩:每 ~30 帧一次(节流,勿刷屏)。zMin/zMax 无页时归零。
    if (pageDetFrameCounter_ % 30u == 0u) {
        const int logZMin = visiblePagesScratch_.empty() ? 0 : zMin;
        const int logZMax = visiblePagesScratch_.empty() ? 0 : zMax;
        platformLog(LogLevel::Warning, "PageDet",
                    "uniquePages=%d visibleCappedTiles=%d zMin=%d zMax=%d maxTileSse=%.0f",
                    lastVisiblePageCount_, visibleCappedTiles, logZMin, logZMax,
                    maxTileSse);
    }
}

TerrainPageStore::~TerrainPageStore() {
    for (auto& [packed, entry] : entries_) {
        entry.fetchToken.cancel();  // 尽力取消在途 fetch(回调仍安全:只写 shared inbox)
    }
}

bool TerrainPageStore::initialize(RenderDevice* device, const Config& config) {
    if (!device) {
        return false;
    }
    device_ = device;
    config_ = config;
    config_.pageSizeTexels = std::max(1, config_.pageSizeTexels);
    config_.depthLevels = std::clamp(config_.depthLevels, 0, 4);
    config_.maxResidentTiles = std::max(1, config_.maxResidentTiles);
    config_.maxUploadsPerFrame = std::max(1, config_.maxUploadsPerFrame);
    gridN_ = 1 << config_.depthLevels;
    const int blockLayers = gridN_ * gridN_;
    const int totalLayers = blockLayers * config_.maxResidentTiles;

    // 共享 texture2DArray:pageSize×pageSize×totalLayers,RGBA8,逐层 CLAMP_TO_EDGE
    // (§13.1 无页缝),linear(真实影像放大平滑)。
    TextureDesc desc;
    desc.width = config_.pageSizeTexels;
    desc.height = config_.pageSizeTexels;
    desc.arrayLayers = totalLayers;
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
    pool_.configure(config_.maxResidentTiles, blockLayers);
    inbox_ = std::make_shared<PendingInbox>();
    return true;
}

void TerrainPageStore::eraseEntry(uint64_t packed) {
    const auto it = entries_.find(packed);
    if (it == entries_.end()) {
        return;
    }
    it->second.fetchToken.cancel();  // 在途 fetch 作废(到达也会被 drain 校验丢弃)
    entries_.erase(it);
}

void TerrainPageStore::applyToTerrainCommand(
    RenderCommand& cmd, const TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& overlays) {
    if (!arrayTexture_ || !cmd.terrainRenderContent) {
        return;
    }
    // 只挂真实地形(fill/ellipsoid proxy 不是最终高清目标 → 留 mappedRaster)。
    if (cmd.terrainSurfaceSource != TerrainSurfaceCommandSource::RealTerrain) {
        return;
    }
    // 首帧捕获影像 provider(拉真实高清影像的数据源)。
    if (!provider_ && !overlays.empty()) {
        provider_ = overlays.front()->getTileProvider();
    }

    const uint64_t packed = packKey(tile.key);
    const bool wasResident = pool_.layerBaseFor(packed) >= 0;
    uint64_t evicted = 0;
    const int layerBase = pool_.acquire(packed, frameId_, &evicted);
    if (evicted != 0) {
        eraseEntry(evicted);
    }
    if (layerBase < 0) {
        return;  // 层池本帧满 → 回落 mappedRaster(决策② 共存)
    }

    if (!wasResident) {
        TileEntry entry;
        entry.key = tile.key;
        entry.layerBase = layerBase;
        entry.gridN = gridN_;
        entry.depthLevels = config_.depthLevels;
        entry.bounds = tile.bounds;
        entry.targetZ = tile.key.z;
        // Step B1:建 per-tile 间接纹理(dense 填,layerBase/gridN 存活期固定 →
        // 一次填好)。片元经它单次 NEAREST fetch 定位 array 层。
        entry.indirTexture = buildIndirTexture(layerBase, gridN_);
        entries_[packed] = std::move(entry);
        // 新块不灌占位:决策② 共存下,页存储只在**全部** gridN² 层就绪后才 enabled,
        // 未就绪期走 mappedRaster fallback → 占位层永不被采样(灌了也是死上传)。
    }

    const TileEntry& entry = entries_[packed];
    // 决策② 共存:仅在该瓦片**整块**层就绪后接管(避免上一租户残留渗入未到达 cell);
    // 未就绪 → 不动 → mappedRaster fallback(优雅降级,§12.5#4)。
    if (entry.uploadedLayers < entry.gridN * entry.gridN) {
        return;
    }
    // Step B1:间接纹理必须就绪才接管(否则片元 fetch 空指针纹理)。dense 填在
    // entry 创建时同步建好,理应非空;防御式短路 → 回落 mappedRaster。
    if (!entry.indirTexture) {
        return;
    }
    if (cmd.textures.size() <=
        static_cast<size_t>(kGltfPageStoreIndirTextureSlot)) {
        cmd.textures.resize(
            static_cast<size_t>(kGltfPageStoreIndirTextureSlot) + 1u, nullptr);
    }
    cmd.textures[kGltfPageStoreArrayTextureSlot] = arrayTexture_.get();
    // Step B1:间接纹理绑 slot21,片元经它 fetch 定位层(替代闭式公式)。
    cmd.textures[kGltfPageStoreIndirTextureSlot] = entry.indirTexture.get();
    // enabled=1、gridN → 片元采页存储。.z(layerBase)现由间接纹理内容承载,
    // shader 不再用(dense 填时间接纹理里编的就是 layerBase+... → 结果等价);
    // 保留写入以最小化 uniform 布局改动。
    cmd.gltfUniforms.pageStoreParams = {1.0f,
                                        static_cast<float>(entry.gridN),
                                        static_cast<float>(entry.layerBase),
                                        0.0f};
}

void TerrainPageStore::tick() {
    if (!arrayTexture_) {
        return;
    }
    ++frameId_;  // 推进帧号(applyToTerrainCommand 用它 touch 本帧可见块)
    // kick 未启动的 per-tile fetch。
    if (provider_) {
        for (auto& [packed, entry] : entries_) {
            if (!entry.fetchKicked) {
                kickImageryFetch(entry);
                entry.fetchKicked = true;
            }
        }
    }
    drainInbox();
}

void TerrainPageStore::kickImageryFetch(TileEntry& entry) {
    ImageryProvider& imagery = provider_->getImageryProvider();
    const TileScheme& scheme = provider_->getTileScheme();
    const int gridN = entry.gridN;
    const int zoom = entry.targetZ + entry.depthLevels;
    if (zoom > provider_->getMaximumLevel()) {
        // 影像深度不足以铺 gridN×gridN 对齐子瓦片:跳过(留占位,不做部分覆盖免错位)。
        return;
    }
    // **mercator 直接子瓦片**(§13.4 修 lat-linear 枚举错位):terrain 与影像同 XYZ
    // web-mercator 分块,故目标瓦片 (z,x,y) 的 z+depth 子瓦片 =
    // (x*gridN+dx, y*gridN+dy),与 shader mercator UV 的 cell 网格逐格对齐
    // (NW 约定 v=0 北 → cell.y=0=北=最小 y)。schemeId 经 positionToTile(瓦片中心)取。
    const double centerLng = 0.5 * (entry.bounds.west() + entry.bounds.east());
    const double centerLat = 0.5 * (entry.bounds.south() + entry.bounds.north());
    const TileKey centerImg =
        scheme.positionToTile(centerLng, centerLat, entry.targetZ);
    const uint64_t packed = packKey(entry.key);
    const int layerBase = entry.layerBase;
    for (int dy = 0; dy < gridN; ++dy) {
        for (int dx = 0; dx < gridN; ++dx) {
            TileKey childKey;
            childKey.schemeId = centerImg.schemeId;
            childKey.z = zoom;
            childKey.x = entry.key.x * gridN + dx;
            childKey.y = entry.key.y * gridN + dy;
            const int layer = layerBase + dy * gridN + dx;  // 与 shader cell.y*gridN+cell.x 对齐
            std::shared_ptr<PendingInbox> inbox = inbox_;
            imagery.requestTile(
                childKey, entry.fetchToken,
                [inbox, packed, layer](const TileKey&,
                                       std::unique_ptr<DecodedImage> image) {
                    if (!image) return;
                    std::lock_guard<std::mutex> lock(inbox->mutex);
                    inbox->pages.push_back({packed, layer, std::move(image)});
                });
        }
    }
}

void TerrainPageStore::drainInbox() {
    std::vector<PendingInbox::Item> ready;
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
    int uploaded = 0;
    size_t requeueFrom = 0;
    for (size_t idx = 0; idx < ready.size(); ++idx) {
        if (uploaded >= config_.maxUploadsPerFrame) {
            requeueFrom = idx;  // 超本帧预算:剩余下帧再传(涓流,勿冻结拖动)
            break;
        }
        auto& item = ready[idx];
        requeueFrom = idx + 1;
        DecodedImage* image = item.image.get();
        if (!image || image->width != side || image->height != side ||
            image->pixels.empty()) {
            continue;  // 尺寸不符(非 256²)跳过,保留占位
        }
        // 校验 entry 仍驻留且该 layer 属于它(淘汰/换租后 layerBase 变 → 丢弃)。
        const auto it = entries_.find(item.key);
        if (it == entries_.end()) {
            continue;  // entry 已淘汰
        }
        TileEntry& entry = it->second;
        if (item.layer < entry.layerBase ||
            item.layer >= entry.layerBase + entry.gridN * entry.gridN) {
            continue;  // 该 layer 已不属于此 entry(换租)
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
                                     static_cast<size_t>(side) * 4u,
                                     item.layer);
        ++entry.uploadedLayers;
        ++uploadedLayerTotal_;
        ++uploaded;
    }
    // 未处理完的项(超预算)放回 inbox 下帧继续。
    if (requeueFrom < ready.size()) {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        for (size_t idx = requeueFrom; idx < ready.size(); ++idx) {
            inbox_->pages.push_back(std::move(ready[idx]));
        }
    }
}

}  // namespace earth_engine
