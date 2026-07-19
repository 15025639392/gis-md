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
#include "../tiling/TileSelectionInputMetrics.h"
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

// thick-OBB 过取收紧(§15.3①):capped 父瓦片高度带很厚(可上千米),z17 子 cell
// 仅 ~150m 宽,套厚 OBB 在掠射/边缘会假阳性戳进视锥。视锥内再按 cell 自身屏幕误差
// 二次剔除——SSE 远小于地形细化阈值的 cell 屏幕贡献可忽略(本应由 coarser 页服务,
// 当前单-父瓦片 zoom 无法区分),丢弃以逼近 #3 屏幕工作集(近 68/地平线 185)。
// 阈值取地形 SSE 阈值的一半(§15.3 建议):nadir cell SSE≈阈值,丢弃 >2× 距离低贡献 cell。
constexpr double kCellSseCullFraction = 0.5;

}  // namespace

// worker 回调把解码影像投进本箱(shared_ptr 持有 → 即使 TerrainPageStore 已析构
// 也不悬垂);渲染线程 drainInbox 取走上传。每项带 pageKey + 目标 layer,
// drain 时校验页仍驻留于该 layer(淘汰/换租后到达的丢弃)。
struct TerrainPageStore::PendingInbox {
    struct Item {
        uint64_t key = 0;  // pageKey(packKey of 影像页 z/x/y)
        int layer = 0;     // 目标 array 层(kick 时 pool 分配的 layer)
        std::unique_ptr<DecodedImage> image;
    };
    std::mutex mutex;
    std::vector<Item> pages;
};

void TerrainPageStore::encodeLayerRGBA8(int layer, bool resident, uint8_t out[4]) {
    const unsigned int v = static_cast<unsigned int>(std::max(0, layer));
    out[0] = static_cast<uint8_t>(v & 0xFFu);          // R = layer & 0xFF
    out[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);   // G = (layer >> 8) & 0xFF
    out[2] = 0;                                         // B 保留
    // A = resident 标志(B2b):片元 alphaOver factor。resident=页已 uploaded → 覆盖;
    // miss(视锥外/未 fetch/未到)→ 0 → 保留 base=mappedRaster(优雅降级不出洞)。
    out[3] = resident ? 255 : 0;
}

int TerrainPageStore::decodeLayerRGBA8(const uint8_t in[4]) {
    // 逐位镜像片元 shader:floor(r*255+0.5)+floor(g*255+0.5)*256(RGBA8 unorm
    // 采样后 r=R/255,floor(R+0.5)=R)。
    return static_cast<int>(in[0]) + static_cast<int>(in[1]) * 256;
}

std::unique_ptr<Texture> TerrainPageStore::createIndirTexture(
    int gridN, const uint8_t* texels) {
    // gridN×gridN RGBA8 稀疏间接纹理,初值 = 本帧填好的 texels(NW 无翻转:cy=0=北=
    // row0=最小 v,和影像上传 kFlipRowsOnUpload=false + shader cell.y 一致)。片元
    // (cell+0.5)/gridN 命中 texel 中心 → NEAREST 取精确 layer + A 通道。后续帧经
    // updateTextureRegion 原地刷新(两后端 create-with-data 与 region 更新行序一致)。
    TextureDesc desc;
    desc.width = gridN;
    desc.height = gridN;
    desc.arrayLayers = 1;  // 普通 2D(非 array):间接纹理是索引表,非页存储
    desc.format = TextureDesc::Format::RGBA8;
    desc.data = texels;
    desc.dataSize = static_cast<size_t>(gridN) * static_cast<size_t>(gridN) * 4u;
    desc.mipmap = false;
    // NEAREST:间接纹理必须点采样取精确 texel(线性会在 cell 边界串值→错 layer/A)。
    desc.minFilter = TextureDesc::Filter::Nearest;
    desc.magFilter = TextureDesc::Filter::Nearest;
    desc.wrapS = TextureDesc::Wrap::Clamp;
    desc.wrapT = TextureDesc::Wrap::Clamp;
    return device_->createTexture(desc);
}

uint64_t TerrainPageStore::packKey(const TileKey& key) {
    // B2b:页 = 影像瓦片,z ≤ 17,x/y < 2^17 < 2^22 → 无损打包进 64 位。schemeId 不入
    // key(单一影像 provider,z/x/y 唯一定位页)。也复用于 capped 瓦片 tileKey。
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
    if (!arrayTexture_ || !provider || visibleTiles.empty()) {
        return;
    }
    if (!provider_) {
        provider_ = provider;  // determination 也可首次捕获(供 kickPageFetch 用)
    }
    const TileScheme& scheme = provider->getTileScheme();
    const int providerMaxLevel = provider->getMaximumLevel();

    visiblePagesScratch_.clear();
    int visibleCappedTiles = 0;
    int zMin = std::numeric_limits<int>::max();
    int zMax = std::numeric_limits<int>::min();
    double maxTileSse = 0.0;  // TEMP 诊断:本帧最大瓦片屏幕 SSE(驱动 zoom)
    int culledBySse = 0;      // TEMP 诊断:被 screen-SSE 过滤剔除的 cell 数(§15.3①)

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

        const int gridN = subtileGridN(tile->key.z, zoom);
        const double minH = TileBoundsMetrics::terrainMinimumHeight(*tile);
        const double maxH = TileBoundsMetrics::terrainMaximumHeight(*tile);

        // 3) 分配/复用该瓦片的稀疏间接纹理(gridN 变则重建)。逐 cell 填 resident/miss。
        const uint64_t tileKeyPacked = packKey(tile->key);
        TileIndir& ind = tileIndirs_[tileKeyPacked];
        indirTexelsScratch_.assign(
            static_cast<size_t>(gridN) * static_cast<size_t>(gridN) * 4u, 0);

        // 影像 fetch key 需影像 provider 的 schemeId(terrain tileKey.schemeId 可能异号):
        // 由影像 scheme.positionToTile(瓦片中心)取,x/y/z 沿用共享 XYZ 网格。
        const Rectangle tileRect = scheme.tileToRectangle(tile->key);
        const double cLng = 0.5 * (tileRect.west() + tileRect.east());
        const double cLat = 0.5 * (tileRect.south() + tileRect.north());
        const auto imgSchemeId =
            scheme.positionToTile(cLng, cLat, tile->key.z).schemeId;

        for (int dy = 0; dy < gridN; ++dy) {
            for (int dx = 0; dx < gridN; ++dx) {
                uint8_t* texel = indirTexelsScratch_.data() +
                                 (static_cast<size_t>(dy) * gridN + dx) * 4u;
                TileKey sub;
                sub.schemeId = tile->key.schemeId;  // 视锥/rect 计算沿用 terrain scheme
                sub.z = zoom;
                sub.x = tile->key.x * gridN + dx;
                sub.y = tile->key.y * gridN + dy;
                const Rectangle subRect = scheme.tileToRectangle(sub);
                const std::optional<OrientedBoundingBox> obb =
                    TileBoundsMetrics::boundingRegionObb(subRect, minH, maxH);
                if (!obb || !view.frustum.intersectsOBB(*obb)) {
                    encodeLayerRGBA8(0, false, texel);  // 视锥外 → miss(mappedRaster)
                    continue;
                }
                // thick-OBB 过取收紧(§15.3①):视锥内再按 cell 自身屏幕误差二次剔除。
                // 子 geomError = 父/2^depth = 父/gridN(四叉树每级半);dist=相机到 cell
                // OBB 最近点。SSE < 阈值*fraction → 屏幕贡献过小(厚 OBB 假阳性/远景掠射)
                // → 丢弃当 miss(回落 mappedRaster,不出洞),逼近屏幕工作集。
                const double subGeomError =
                    tile->geometricError / static_cast<double>(gridN);
                const double cellDist = std::sqrt(
                    obb->computeDistanceSquaredToPosition(view.position));
                const double cellSse =
                    TileSelectionInputMetrics::screenSpaceErrorForView(
                        subGeomError, view.projectionMatrix,
                        view.viewportHeightPixels, cellDist);
                if (cellSse < threshold * kCellSseCullFraction) {
                    encodeLayerRGBA8(0, false, texel);  // 贡献过小 → miss
                    ++culledBySse;
                    continue;
                }
                const uint64_t pageKey = packKey(sub);
                visiblePagesScratch_.insert(pageKey);  // 去重计数(跨瓦片共享祖先)
                zMin = std::min(zMin, zoom);
                zMax = std::max(zMax, zoom);

                // 4) pool.acquire 得 layer(页首次命中建 PageEntry + kick fetch)。
                uint64_t evicted = 0;
                const int layer = pool_.acquire(pageKey, frameId_, &evicted);
                if (evicted != 0) {
                    erasePageEntry(evicted);  // 淘汰页:cancel fetch + 移除账本
                }
                if (layer < 0) {
                    encodeLayerRGBA8(0, false, texel);  // 池本帧满 → miss
                    continue;
                }
                auto [it, inserted] = pages_.try_emplace(pageKey);
                PageEntry& pe = it->second;
                if (inserted) {
                    pe.layer = layer;
                    pe.uploaded = false;
                    TileKey fetchKey;
                    fetchKey.schemeId = imgSchemeId;  // 影像 provider 的 schemeId
                    fetchKey.z = zoom;
                    fetchKey.x = sub.x;
                    fetchKey.y = sub.y;
                    kickPageFetch(fetchKey, pageKey, layer, pe.fetchToken);
                }
                // resident=已 uploaded → A=255 覆盖;否则 A=0 保留 mappedRaster。
                encodeLayerRGBA8(pe.layer, pe.uploaded, texel);
            }
        }

        // gridN 变(或首见)→ 建纹理(带初值);否则原地刷新(每帧 resident 变化)。
        if (!ind.tex || ind.gridN != gridN) {
            ind.tex = createIndirTexture(gridN, indirTexelsScratch_.data());
            ind.gridN = gridN;
        } else {
            device_->updateTextureRegion(
                ind.tex.get(), 0, 0, gridN, gridN, indirTexelsScratch_.data(),
                static_cast<size_t>(gridN) * 4u, 0);
        }
        ind.lastFrame = frameId_;
    }

    // sweep:清除本帧不再可见的瓦片间接纹理(其页经 LRU 自然淘汰)。
    for (auto it = tileIndirs_.begin(); it != tileIndirs_.end();) {
        if (it->second.lastFrame != frameId_) {
            it = tileIndirs_.erase(it);
        } else {
            ++it;
        }
    }

    lastVisiblePageCount_ = static_cast<int>(visiblePagesScratch_.size());
    // 插桩:每 ~30 帧一次(节流,勿刷屏)。zMin/zMax 无页时归零。
    if (pageDetFrameCounter_ % 30u == 0u) {
        const int logZMin = visiblePagesScratch_.empty() ? 0 : zMin;
        const int logZMax = visiblePagesScratch_.empty() ? 0 : zMax;
        platformLog(LogLevel::Warning, "PageDet",
                    "uniquePages=%d residentPages=%d uploadedTotal=%d "
                    "visibleCappedTiles=%d zMin=%d zMax=%d maxTileSse=%.0f "
                    "culledBySse=%d",
                    lastVisiblePageCount_, pool_.residentCount(),
                    uploadedLayerTotal_, visibleCappedTiles, logZMin, logZMax,
                    maxTileSse, culledBySse);
    }
}

TerrainPageStore::~TerrainPageStore() {
    for (auto& [pageKey, pe] : pages_) {
        pe.fetchToken.cancel();  // 尽力取消在途 fetch(回调仍安全:只写 shared inbox)
    }
}

bool TerrainPageStore::initialize(RenderDevice* device, const Config& config) {
    if (!device) {
        return false;
    }
    device_ = device;
    config_ = config;
    config_.pageSizeTexels = std::max(1, config_.pageSizeTexels);
    config_.maxPages = std::max(1, config_.maxPages);
    config_.maxUploadsPerFrame = std::max(1, config_.maxUploadsPerFrame);

    // 共享 texture2DArray:pageSize×pageSize×maxPages(每页一层,blockLayers=1),
    // RGBA8,逐层 CLAMP_TO_EDGE(§13.1 无页缝),linear(真实影像放大平滑)。
    TextureDesc desc;
    desc.width = config_.pageSizeTexels;
    desc.height = config_.pageSizeTexels;
    desc.arrayLayers = config_.maxPages;
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
    pool_.configure(config_.maxPages, /*blockLayers=*/1);
    inbox_ = std::make_shared<PendingInbox>();
    return true;
}

void TerrainPageStore::erasePageEntry(uint64_t pageKey) {
    const auto it = pages_.find(pageKey);
    if (it == pages_.end()) {
        return;
    }
    it->second.fetchToken.cancel();  // 在途 fetch 作废(到达也会被 drain 校验丢弃)
    pages_.erase(it);
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
    // 首帧捕获影像 provider(拉真实高清影像的数据源;determination 通常已捕获)。
    if (!provider_ && !overlays.empty()) {
        provider_ = overlays.front()->getTileProvider();
    }

    // B2b:无相机,只 bind determination 本帧建好的稀疏间接纹理。无 TileIndir
    // (未 determined / 无可见页)→ 不动 → mappedRaster(决策② 共存,零回归)。
    const auto it = tileIndirs_.find(packKey(tile.key));
    if (it == tileIndirs_.end() || !it->second.tex) {
        return;
    }
    const TileIndir& ind = it->second;
    if (cmd.textures.size() <=
        static_cast<size_t>(kGltfPageStoreIndirTextureSlot)) {
        cmd.textures.resize(
            static_cast<size_t>(kGltfPageStoreIndirTextureSlot) + 1u, nullptr);
    }
    cmd.textures[kGltfPageStoreArrayTextureSlot] = arrayTexture_.get();
    // 间接纹理绑 slot21,片元经它 fetch 定位 layer + 读 A 通道作 miss 回退 factor。
    cmd.textures[kGltfPageStoreIndirTextureSlot] = ind.tex.get();
    // enabled=1、gridN → 片元采页存储。layer 由间接纹理 RG 承载,resident/miss 由
    // 其 A 承载;pageStoreParams.z/.w 不再用(留 0)。
    cmd.gltfUniforms.pageStoreParams = {1.0f, static_cast<float>(ind.gridN),
                                        0.0f, 0.0f};
}

void TerrainPageStore::tick() {
    if (!arrayTexture_) {
        return;
    }
    ++frameId_;  // 推进帧号(下帧 determination 的 LRU touch/淘汰基准)
    drainInbox();  // fetch 已在 determination 页首次命中时 kick
}

void TerrainPageStore::kickPageFetch(const TileKey& pageTileKey,
                                     uint64_t pageKey, int layer,
                                     CancellationToken& token) {
    if (!provider_) {
        return;
    }
    ImageryProvider& imagery = provider_->getImageryProvider();
    std::shared_ptr<PendingInbox> inbox = inbox_;
    imagery.requestTile(
        pageTileKey, token,
        [inbox, pageKey, layer](const TileKey&,
                                std::unique_ptr<DecodedImage> image) {
            if (!image) return;
            std::lock_guard<std::mutex> lock(inbox->mutex);
            inbox->pages.push_back({pageKey, layer, std::move(image)});
        });
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
        // 校验页仍驻留且 layer 匹配(淘汰/换租后 layer 变或页消失 → 丢弃,防写错层)。
        const auto it = pages_.find(item.key);
        if (it == pages_.end()) {
            continue;  // 页已淘汰(erasePageEntry)
        }
        PageEntry& pe = it->second;
        if (pe.layer != item.layer) {
            continue;  // 该 layer 已换租给别的页
        }
        if (pe.uploaded) {
            continue;  // 已灌过(重复 fetch 到达)→ 跳过
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
        pe.uploaded = true;  // 下帧 determination 重建 indir 时该 cell 变 resident
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
