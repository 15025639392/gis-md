#include "TerrainPageStore.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

#include "../core/math/OrientedBoundingBox.h"
#include "../debug/Contracts.h"
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
// PageSourceAssembler — 单页多源按序 alphaOver 合成(纯 CPU,C-1)
// ============================================================

void PageSourceAssembler::configure(int sourceCount, int sideTexels) {
    sourceCount_ = std::max(0, sourceCount);
    side_ = std::max(0, sideTexels);
    composited_ = 0;
    accum_.clear();
    accum_.shrink_to_fit();
    stash_.assign(static_cast<size_t>(sourceCount_), std::vector<uint8_t>{});
}

void PageSourceAssembler::releaseBuffers() {
    accum_.clear();
    accum_.shrink_to_fit();
    stash_.assign(static_cast<size_t>(sourceCount_), std::vector<uint8_t>{});
}

namespace {

/// 直通(非预乘)alpha 的 source-over:src 叠在 dst 上,就地写 dst。
/// out.a = sa + da(1-sa);out.rgb = (src·sa + dst·da·(1-sa)) / out.a。
/// out.a==0 时 rgb 无意义,置 0 保证确定性(否则除零)。
void alphaOverStraightInPlace(uint8_t* dst, const uint8_t* src, size_t texels) {
    for (size_t i = 0; i < texels; ++i) {
        const float sa = src[3] * (1.0f / 255.0f);
        const float da = dst[3] * (1.0f / 255.0f);
        const float inv = 1.0f - sa;
        const float oa = sa + da * inv;
        if (oa <= 0.0f) {
            dst[0] = dst[1] = dst[2] = dst[3] = 0;
        } else {
            const float dw = da * inv;
            for (int c = 0; c < 3; ++c) {
                const float v = (src[c] * sa + dst[c] * dw) / oa;
                dst[c] = static_cast<uint8_t>(
                    std::min(255.0f, std::max(0.0f, v + 0.5f)));
            }
            dst[3] = static_cast<uint8_t>(
                std::min(255.0f, std::max(0.0f, oa * 255.0f + 0.5f)));
        }
        dst += 4;
        src += 4;
    }
}

}  // namespace

bool PageSourceAssembler::accept(int sourceIndex, const uint8_t* rgba) {
    if (sourceCount_ <= 0 || side_ <= 0 || rgba == nullptr) {
        return false;
    }
    if (sourceIndex < 0 || sourceIndex >= sourceCount_) {
        return false;
    }
    if (sourceIndex < composited_) {
        return false;  // 该源已合成(重复到达)→ 幂等丢弃,勿写两遍
    }
    const size_t bytes =
        static_cast<size_t>(side_) * static_cast<size_t>(side_) * 4u;
    if (sourceIndex > composited_) {
        // 乱序早到:暂存,等前序源补齐再按序合成(alphaOver 不可交换)。
        std::vector<uint8_t>& slot = stash_[static_cast<size_t>(sourceIndex)];
        if (slot.empty()) {
            slot.assign(rgba, rgba + bytes);
        }
        return false;
    }
    // sourceIndex == composited_:按序合成,随后把 stash 里已连上的后续源一起消化。
    const uint8_t* next = rgba;
    std::vector<uint8_t> pending;
    while (next != nullptr) {
        if (composited_ == 0) {
            accum_.assign(next, next + bytes);  // 首源直接拷贝(单源逐字节等价)
        } else {
            alphaOverStraightInPlace(
                accum_.data(), next,
                static_cast<size_t>(side_) * static_cast<size_t>(side_));
        }
        ++composited_;
        next = nullptr;
        if (composited_ < sourceCount_) {
            std::vector<uint8_t>& slot = stash_[static_cast<size_t>(composited_)];
            if (!slot.empty()) {
                pending.swap(slot);
                slot.clear();
                slot.shrink_to_fit();
                next = pending.data();
            }
        }
    }
    return true;
}

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

void TerrainPageLayerPool::touch(uint64_t key, uint64_t frameId) {
    const auto it = keyToSlot_.find(key);
    if (it == keyToSlot_.end()) {
        return;  // 不驻留 → no-op(不像 acquire 那样分配/淘汰)
    }
    slots_[static_cast<size_t>(it->second)].lastFrame = frameId;
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

// per-cell 渐变 LOD 的真 miss 地板(§16.3⑥,替代 §15.3① 的 0.5 硬剔)。
// §15.3① 曾按 cell 屏幕误差硬剔到 0.5×阈值——但被剔的中距 cell 一步跌回 z12
// mappedRaster(5 级悬崖)= 高倾斜「模糊带」根因。§16.3 改为:视锥内 cell 按距离取
// 自适应粗祖先页(渐变),仅保留一层**更低**的地板兜厚 OBB 掠射假阳性——屏幕贡献
// < 1/4 阈值的 cell(远景/掠射误戳)才真 miss 回落 mappedRaster,防 near-nadir 枚举爆量。
constexpr double kCellSseMissFloorFraction = 0.25;

}  // namespace

// worker 回调把解码影像投进本箱(shared_ptr 持有 → 即使 TerrainPageStore 已析构
// 也不悬垂);渲染线程 drainInbox 取走上传。每项带 pageKey + 目标 layer,
// drain 时校验页仍驻留于该 layer(淘汰/换租后到达的丢弃)。
struct TerrainPageStore::PendingInbox {
    struct Item {
        uint64_t key = 0;  // pageKey(packKey of 影像页 z/x/y)
        int layer = 0;     // 目标 array 层(kick 时 pool 分配的 layer)
        int source = 0;    // C-1:有序 provider 列表中的源号(决定合成次序)
        // C-1b:该源被钳到自己 maxZoom 后的祖先深度与页在祖先内的格位。
        // depth=0 = 拉到了本页本级(现状);>0 = 拿的是祖先页,合成时取
        // (subX,subY)/2^depth 那块子矩形放大。
        int ancestorDepth = 0;
        int subX = 0;
        int subY = 0;
        std::unique_ptr<DecodedImage> image;
    };
    std::mutex mutex;
    std::vector<Item> pages;
};

void TerrainPageStore::resamplePageSource(const DecodedImage& image, int depth,
                                          int subX, int subY, int side,
                                          std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(side) * static_cast<size_t>(side) * 4u, 0);
    if (image.width <= 0 || image.height <= 0 || image.pixels.empty() ||
        side <= 0) {
        return;
    }
    const int ch = std::max(1, image.channels);
    const int iw = image.width;
    const int ih = image.height;
    const uint8_t* src = image.pixels.data();
    // 页在祖先内的归一化子矩形:origin = sub/2^depth,边长 = 1/2^depth。
    const float span = 1.0f / static_cast<float>(1 << std::max(0, depth));
    const float u0 = static_cast<float>(subX) * span;
    const float v0 = static_cast<float>(subY) * span;
    // depth=0 且 image 尺寸 == side 时,下面的 0.5 偏移相消 → 逐像素恰好落在
    // 源像素中心 → 双线性权重为 0 = 逐字节直拷(与 C-1b 之前等价)。
    auto sample = [&](float fx, float fy, uint8_t* d) {
        const int x0 = static_cast<int>(std::floor(fx));
        const int y0 = static_cast<int>(std::floor(fy));
        const float tx = fx - static_cast<float>(x0);
        const float ty = fy - static_cast<float>(y0);
        const int xs[2] = {std::min(std::max(x0, 0), iw - 1),
                           std::min(std::max(x0 + 1, 0), iw - 1)};
        const int ys[2] = {std::min(std::max(y0, 0), ih - 1),
                           std::min(std::max(y0 + 1, 0), ih - 1)};
        float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (int j = 0; j < 2; ++j) {
            const float wy = j == 0 ? (1.0f - ty) : ty;
            for (int i = 0; i < 2; ++i) {
                const float w = (i == 0 ? (1.0f - tx) : tx) * wy;
                if (w <= 0.0f) continue;
                const uint8_t* s =
                    src + (static_cast<size_t>(ys[j]) * iw + xs[i]) * ch;
                if (ch >= 3) {
                    acc[0] += w * s[0];
                    acc[1] += w * s[1];
                    acc[2] += w * s[2];
                    acc[3] += w * (ch >= 4 ? s[3] : 255);
                } else {  // 单通道:灰度铺三通道
                    acc[0] += w * s[0];
                    acc[1] += w * s[0];
                    acc[2] += w * s[0];
                    acc[3] += w * 255.0f;
                }
            }
        }
        for (int c = 0; c < 4; ++c) {
            d[c] = static_cast<uint8_t>(
                std::min(255.0f, std::max(0.0f, acc[c] + 0.5f)));
        }
    };
    for (int row = 0; row < side; ++row) {
        const int outRow = kFlipRowsOnUpload ? (side - 1 - row) : row;
        const float v = v0 + (static_cast<float>(row) + 0.5f) /
                                 static_cast<float>(side) * span;
        const float fy = v * static_cast<float>(ih) - 0.5f;
        uint8_t* d = out.data() + static_cast<size_t>(outRow) * side * 4;
        for (int col = 0; col < side; ++col) {
            const float u = u0 + (static_cast<float>(col) + 0.5f) /
                                     static_cast<float>(side) * span;
            sample(u * static_cast<float>(iw) - 0.5f, fy, d);
            d += 4;
        }
    }
}

void TerrainPageStore::encodeLayerRGBA8(int layer, bool resident, int depth,
                                        uint8_t out[4]) {
    const unsigned int v = static_cast<unsigned int>(std::max(0, layer));
    out[0] = static_cast<uint8_t>(v & 0xFFu);          // R = layer & 0xFF
    out[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);   // G = (layer >> 8) & 0xFF
    // B = per-cell 渐变 LOD 深度 d(§16.3),clamp [0, kMaxDetDepthLevels]。片元
    // span=2^d 定位粗祖先页子区;d=0 逐字节=现状精页。
    out[2] = static_cast<uint8_t>(std::clamp(depth, 0, kMaxDetDepthLevels));
    // A = resident 标志(B2b):片元 alphaOver factor。resident=页已 uploaded → 覆盖;
    // miss(视锥外/未 fetch/未到)→ 0 → 保留 base=mappedRaster(优雅降级不出洞)。
    out[3] = resident ? 255 : 0;
}

int TerrainPageStore::decodeLayerRGBA8(const uint8_t in[4]) {
    // 逐位镜像片元 shader:floor(r*255+0.5)+floor(g*255+0.5)*256(RGBA8 unorm
    // 采样后 r=R/255,floor(R+0.5)=R)。
    return static_cast<int>(in[0]) + static_cast<int>(in[1]) * 256;
}

int TerrainPageStore::decodeDepthRGBA8(const uint8_t in[4]) {
    return static_cast<int>(in[2]);  // 镜像片元 floor(b*255+0.5)
}

TileKey TerrainPageStore::unpackKey(uint64_t packed) {
    TileKey key;
    key.z = static_cast<int>(packed >> 44);
    key.x = static_cast<int>((packed >> 22) & 0x3FFFFFull);
    key.y = static_cast<int>(packed & 0x3FFFFFull);
    return key;  // schemeId 不入 key(见 packKey),保持缺省
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
    const std::vector<RasterOverlayTileProvider*>& providers,
    double terrainMaxScreenSpaceError) {
    ++pageDetFrameCounter_;
    // 逐帧重置合批资格判因:不重置的话 worstResidentRatio_ 会单调下降成历史最差值,
    // 报的就不再是"此刻",而 A/B 里最怕的正是把陈旧值当当前值读。
    residencyCheckedTiles_ = 0;
    fullyResidentTiles_ = 0;
    worstResidentRatio_ = 1.0f;
    if (!arrayTexture_ || providers.empty() || providers.front() == nullptr ||
        visibleTiles.empty()) {
        return;
    }
    // C-1:源列表变了(增删/换序)→ 已合成的页少一层或多一层都是错的,全部作废重来。
    // 逐指针精确比对(与 det 签名同样的「宁可多跑不可用错」取向)。
    // null 源必须剔除:它永远不会到达,会把 assembler 的按序游标永久卡住。
    detProvidersScratch_.clear();
    for (RasterOverlayTileProvider* p : providers) {
        if (p != nullptr) detProvidersScratch_.push_back(p);
    }
    if (providers_ != detProvidersScratch_) {
        providers_ = detProvidersScratch_;
        for (auto& [key, entry] : pages_) {
            for (CancellationToken& token : entry.fetchTokens) {
                token.cancel();
            }
        }
        pages_.clear();
        // pool_ 的槽位不清:key 仍驻留 → 下次 acquire 返回同一 layer、try_emplace 报
        // inserted → 按新源列表重新 kick。槽位有界不泄漏,且省一轮 LRU 抖动。
    }
    RasterOverlayTileProvider* provider = providers.front();
    const TileScheme& scheme = provider->getTileScheme();
    const int providerMaxLevel = provider->getMaximumLevel();

    // ===== Phase 1:构建 per-tile 参数 + determination 输入签名(便宜,无逐 cell)。
    // zoom = 屏幕合适影像源 LOD = 相机相关(tileSSE 已烘距离;分母=地形阈值 16 匹配
    // 「未 cap 会细化到的几何 LOD」;用 overlay MSE 2 会过取 z18)。=====
    const double threshold = std::max(1e-3, terrainMaxScreenSpaceError);
    detParamsScratch_.clear();
    detTilesScratch_.clear();
    double maxTileSse = 0.0;  // TEMP 诊断
    for (TilesetTile* tile : visibleTiles) {
        if (!tile) {
            continue;
        }
        // 只算真实地形瓦片(与 draw 命令构建同一判定,单一事实源)。
        if (terrainSurfaceSourceForDraw(tile->content.renderContent) !=
            TerrainSurfaceCommandSource::RealTerrain) {
            continue;
        }
        const double tileSse = tile->selectionFrameState.screenSpaceError;
        maxTileSse = std::max(maxTileSse, tileSse);
        int zoom = tile->key.z;
        if (tileSse > threshold) {
            zoom = tile->key.z +
                   static_cast<int>(std::lround(std::log2(tileSse / threshold)));
        }
        zoom = std::min(zoom, providerMaxLevel);
        zoom = std::min(zoom, tile->key.z + kMaxDetDepthLevels);
        zoom = std::max(zoom, tile->key.z);

        const int gridN = subtileGridN(tile->key.z, zoom);
        const double minH = TileBoundsMetrics::terrainMinimumHeight(*tile);
        const double maxH = TileBoundsMetrics::terrainMaximumHeight(*tile);
        const uint64_t tileKeyPacked = packKey(tile->key);
        // 影像 fetch schemeId(terrain tileKey.schemeId 可能异号)由影像 scheme 取。
        const Rectangle tileRect = scheme.tileToRectangle(tile->key);
        const double cLng = 0.5 * (tileRect.west() + tileRect.east());
        const double cLat = 0.5 * (tileRect.south() + tileRect.north());
        const SchemeId imgSchemeId =
            scheme.positionToTile(cLng, cLat, tile->key.z).schemeId;

        detParamsScratch_.push_back(
            {tile, tileKeyPacked, zoom, gridN, minH, maxH, imgSchemeId});
        detTilesScratch_.push_back({tileKeyPacked, zoom, minH, maxH});
    }

    // ===== Phase 2:签名逐字段精确比对(任一不同 → miss,全部瓦片 re-walk 几何)。
    // 精确 ==(非 hash)杜绝「视图变了却判成未变」的雷。几何输入 = frustum 6 平面 +
    // position + projection + viewportHeight + threshold + provider + 瓦片集。=====
    double curPlanes[24];
    {
        using PI = Frustum::PlaneIndex;
        const PI order[6] = {PI::Left, PI::Right,  PI::Bottom,
                             PI::Top,  PI::Near,   PI::Far};
        for (int i = 0; i < 6; ++i) {
            const FrustumPlane& pl = view.frustum.plane(order[i]);
            curPlanes[i * 4 + 0] = pl.normal.x();
            curPlanes[i * 4 + 1] = pl.normal.y();
            curPlanes[i * 4 + 2] = pl.normal.z();
            curPlanes[i * 4 + 3] = pl.distance;
        }
    }
    double curProj[16];
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            curProj[c * 4 + r] = view.projectionMatrix(r, c);
        }
    }
    bool viewMatch = detSigValid_ && detProvider_ == provider &&
                     detVpH_ == view.viewportHeightPixels &&
                     detThreshold_ == threshold &&
                     detPos_[0] == view.position.x() &&
                     detPos_[1] == view.position.y() &&
                     detPos_[2] == view.position.z();
    for (int i = 0; i < 24 && viewMatch; ++i) {
        if (detPlanes_[i] != curPlanes[i]) viewMatch = false;
    }
    for (int i = 0; i < 16 && viewMatch; ++i) {
        if (detProj_[i] != curProj[i]) viewMatch = false;
    }
    const bool hit = viewMatch && detTilesPrev_ == detTilesScratch_;

    // ===== Phase 3:逐瓦片 —— miss 时 walk 几何填缓存;两路都跑驻留编码(每帧必跑,
    // 故异步到页逐帧点亮无 stale)。=====
    visiblePagesScratch_.clear();
    int zMin = std::numeric_limits<int>::max();
    int zMax = std::numeric_limits<int>::min();
    int culledBySse = hit ? lastCulledBySse_ : 0;
    const int visibleCappedTiles = static_cast<int>(detParamsScratch_.size());

    for (const DetTileParam& p : detParamsScratch_) {
        DetTileCacheEntry& cache = detTileCache_[p.tileKeyPacked];
        // 几何 walk 仅在 miss(或 gridN 变=几何变的保险)时跑。
        if (!hit || cache.gridN != p.gridN) {
            cache.gridN = p.gridN;
            cache.kept.clear();
            for (int dy = 0; dy < p.gridN; ++dy) {
                for (int dx = 0; dx < p.gridN; ++dx) {
                    TileKey sub;
                    sub.schemeId = p.tile->key.schemeId;  // 视锥/rect 用 terrain scheme
                    sub.z = p.zoom;
                    sub.x = p.tile->key.x * p.gridN + dx;
                    sub.y = p.tile->key.y * p.gridN + dy;
                    const Rectangle subRect = scheme.tileToRectangle(sub);
                    const std::optional<OrientedBoundingBox> obb =
                        TileBoundsMetrics::boundingRegionObb(subRect, p.minH,
                                                             p.maxH);
                    if (!obb || !view.frustum.intersectsOBB(*obb)) {
                        continue;  // 视锥外 → 不入 kept(编码时默认 miss)
                    }
                    // per-cell 渐变 LOD(§16.3):用**瓦片级** geomError 在 cell 距离处的
                    // 屏幕误差 → cell 该细化到的影像 zoom Za。近 cell(cellDist≈瓦片距)→
                    // cellSse≈tileSse → Za=Z、d=0(逐字节=现状精页);远 cell → Za 渐降
                    // → 采粗祖先页,多远 cell 共享同一粗页(池去重)→ 工作集有界。替代
                    // §15.3① 的硬剔到 z12 悬崖(=模糊带根因)为随距离单调 ≤1 级渐变。
                    const double cellDist = std::sqrt(
                        obb->computeDistanceSquaredToPosition(view.position));
                    const double cellSse =
                        TileSelectionInputMetrics::screenSpaceErrorForView(
                            p.tile->geometricError, view.projectionMatrix,
                            view.viewportHeightPixels, cellDist);
                    // 真 miss 地板(§16.3⑥):屏幕贡献 < 1/4 阈值的 cell = 厚 OBB 掠射
                    // 假阳性,不给页(回落 mappedRaster),防 near-nadir 枚举爆量。
                    if (cellSse < threshold * kCellSseMissFloorFraction) {
                        ++culledBySse;
                        continue;
                    }
                    int za = p.tile->key.z;
                    if (cellSse > threshold) {
                        za = p.tile->key.z +
                             static_cast<int>(
                                 std::lround(std::log2(cellSse / threshold)));
                    }
                    za = std::clamp(za, p.tile->key.z, p.zoom);  // [tileZ, Z]
                    const int d = p.zoom - za;  // 相对精网格下降级数(≥0)
                    // 粗祖先页(zoom=Za):cx=(tileX*gridN+dx)>>d、cy=(...)>>d。
                    // (tileX*gridN 是 2^d 倍数,右移无进位污染,§13.4 对齐推导。)
                    DetKeptCell kc;
                    kc.dx = dx;
                    kc.dy = dy;
                    kc.d = d;
                    const int cx = sub.x >> d;
                    const int cy = sub.y >> d;
                    kc.fetchKey.schemeId = p.imgSchemeId;
                    kc.fetchKey.z = za;
                    kc.fetchKey.x = cx;
                    kc.fetchKey.y = cy;
                    kc.pageKey = packKey(kc.fetchKey);  // 粗页去重 key(同祖先→同 key)
                    cache.kept.push_back(kc);
                }
            }
        }

        // 驻留编码(**每帧必跑**):按当前 resident/uploaded 重建间接纹理。
        // residentCells 统计本帧真正拿到高清页(A=255,含祖先回退)的 cell 数;
        // == gridN² 即「全 cell 驻留」= 合批资格闸(此时 mappedRaster fallback
        // 必不被采样 → 批命令可丢 mappedRaster,见 TerrainInstanceBatcher)。
        indirTexelsScratch_.assign(
            static_cast<size_t>(p.gridN) * static_cast<size_t>(p.gridN) * 4u, 0);
        int residentCells = 0;
        for (const DetKeptCell& kc : cache.kept) {
            uint8_t* texel = indirTexelsScratch_.data() +
                             (static_cast<size_t>(kc.dy) * p.gridN + kc.dx) * 4u;
            visiblePagesScratch_.insert(kc.pageKey);  // 去重计数(粗页共享 → 少)
            zMin = std::min(zMin, kc.fetchKey.z);  // 实际页 zoom = Za(渐变后 ≤ Z)
            zMax = std::max(zMax, kc.fetchKey.z);
            // 请求目标页(za,d)并占槽/touch。
            uint64_t evicted = 0;
            const int layer = pool_.acquire(kc.pageKey, frameId_, &evicted);
            if (evicted != 0) {
                erasePageEntry(evicted);  // 淘汰页:cancel fetch + 移除账本
            }
            if (layer >= 0) {
                auto [it, inserted] = pages_.try_emplace(kc.pageKey);
                PageEntry& pe = it->second;
                if (inserted) {
                    pe.layer = layer;
                    pe.assembler.configure(static_cast<int>(providers_.size()),
                                           config_.pageSizeTexels);
                    kickPageFetches(kc.fetchKey, kc.pageKey, layer, pe);
                }
                // C-1:首源合成上传即算 resident(底图先亮),不等最慢的源。
                if (pe.assembler.hasTexels()) {
                    // 目标页就绪 = 理想 LOD(settled 逐字节=现状精/粗页)。
                    encodeLayerRGBA8(pe.layer, true, kc.d, texel);
                    ++residentCells;
                    continue;
                }
            }
            // 目标未就绪(page-in 中 / 池本帧满):回落**最细的已驻留祖先页**(§16.4
            // 运动抗模糊带)。沿本 cell 祖先链从细到粗(ad 升序=za 降序)找首个 uploaded
            // 页,用它 + 其深度 foundD 采样;touch 保活。运动中相邻距离带的祖先常已驻留
            // (gradient 覆盖 + LRU 保留)→ 显略粗/略细一级而非 z12 mappedRaster 悬崖。
            // 全冷(祖先链无驻留)才 A=0 回落 mappedRaster。settled 目标就绪走上分支不进此。
            const int subX = p.tile->key.x * p.gridN + kc.dx;
            const int subY = p.tile->key.y * p.gridN + kc.dy;
            const int maxAd = p.zoom - p.tile->key.z;  // za=Z 到 tileZ 的最大深度
            int foundLayer = -1;
            int foundD = -1;
            for (int ad = 0; ad <= maxAd; ++ad) {
                TileKey aKey;
                aKey.schemeId = p.imgSchemeId;
                aKey.z = p.zoom - ad;
                aKey.x = subX >> ad;
                aKey.y = subY >> ad;
                const uint64_t aPageKey = packKey(aKey);
                const auto ait = pages_.find(aPageKey);
                if (ait != pages_.end() && ait->second.assembler.hasTexels()) {
                    foundLayer = ait->second.layer;
                    foundD = ad;
                    pool_.touch(aPageKey, frameId_);  // 显示中的祖先页不该被淘汰
                    break;
                }
            }
            if (foundLayer >= 0) {
                encodeLayerRGBA8(foundLayer, true, foundD, texel);
                ++residentCells;
            } else {
                encodeLayerRGBA8(0, false, kc.d, texel);  // 全冷 → mappedRaster
            }
        }

        // 合批 Step 2:认领/保活本瓦片的 array 层,texel 写左上 gridN² 区。
        // 池满(理论上不可能:层数 256 > 峰值可见 ~185)→ 本帧放弃 indir,
        // 该瓦片回落 mappedRaster(优雅降级)。层被夺走的离屏瓦片由 evicted
        // 分支置 layer=-1(其 sweep 稍后清除)。
        TileIndir& ind = tileIndirs_[p.tileKeyPacked];
        if (ind.layer < 0 ||
            indirPool_.layerBaseFor(p.tileKeyPacked) != ind.layer) {
            uint64_t evicted = 0;
            ind.layer = indirPool_.acquire(p.tileKeyPacked, frameId_, &evicted);
            if (evicted != 0) {
                auto eit = tileIndirs_.find(evicted);
                if (eit != tileIndirs_.end()) {
                    eit->second.layer = -1;
                }
            }
        } else {
            indirPool_.touch(p.tileKeyPacked, frameId_);
        }
        if (ind.layer >= 0) {
            device_->updateTextureRegion(
                indirArrayTexture_.get(), 0, 0, p.gridN, p.gridN,
                indirTexelsScratch_.data(),
                static_cast<size_t>(p.gridN) * 4u, ind.layer);
        }
        ind.gridN = p.gridN;
        // 全 cell 驻留(含被 frustum/SSE 剔除的 cell 未 kept → 不计 → 达不到
        // gridN²,该瓦片留逐 draw = 保守但安全,决不丢影像)= 合批资格。
        ind.fullyResident =
            ind.layer >= 0 && residentCells == p.gridN * p.gridN;
        // 合批资格的**唯一**卡点在这里,但此前只有布尔结果、没有距离达标多远的量:
        // 于是"合批为什么一个批都不成形"完全不可查(BatchDet 只能报到
        // notFullyResident 为止)。记下最差覆盖率,把"差一点"与"根本达不到"分开。
        const int cellsNeeded = p.gridN * p.gridN;
        if (cellsNeeded > 0) {
            const float ratio = static_cast<float>(residentCells) /
                                static_cast<float>(cellsNeeded);
            if (ratio < worstResidentRatio_) worstResidentRatio_ = ratio;
            ++residencyCheckedTiles_;
            if (ind.fullyResident) ++fullyResidentTiles_;
        }
        ind.lastFrame = frameId_;
        cache.lastFrame = frameId_;
    }

    // sweep:清本帧不再可见瓦片的间接纹理 + 几何缓存(页经 LRU 自然淘汰)。
    for (auto it = tileIndirs_.begin(); it != tileIndirs_.end();) {
        if (it->second.lastFrame != frameId_) {
            if (it->second.layer >= 0) {
                indirPool_.release(it->first);
            }
            it = tileIndirs_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = detTileCache_.begin(); it != detTileCache_.end();) {
        if (it->second.lastFrame != frameId_) {
            it = detTileCache_.erase(it);
        } else {
            ++it;
        }
    }

    // 保存本帧签名(供下帧 Phase 2 逐值比对)。
    detSigValid_ = true;
    detProvider_ = provider;
    detVpH_ = view.viewportHeightPixels;
    detThreshold_ = threshold;
    detPos_[0] = view.position.x();
    detPos_[1] = view.position.y();
    detPos_[2] = view.position.z();
    for (int i = 0; i < 24; ++i) {
        detPlanes_[i] = curPlanes[i];
    }
    for (int i = 0; i < 16; ++i) {
        detProj_[i] = curProj[i];
    }
    detTilesPrev_ = detTilesScratch_;
    lastCulledBySse_ = culledBySse;

    lastVisiblePageCount_ = static_cast<int>(visiblePagesScratch_.size());
    // 插桩:每 ~30 帧一次(节流,勿刷屏)。zMin/zMax 无页时归零。
    if (pageDetFrameCounter_ % 30u == 0u) {
        const int logZMin = visiblePagesScratch_.empty() ? 0 : zMin;
        const int logZMax = visiblePagesScratch_.empty() ? 0 : zMax;
        // C-1 机制信号:sources=有序源数;complete=全源到齐的页;partial=只到了
        // 前几源的页。partial 长期不降 = 某个源恒不到达(而非「没东西可测」)。
        int completePages = 0;
        int partialPages = 0;
        for (const auto& [pageKey, pe] : pages_) {
            if (pe.assembler.complete()) {
                ++completePages;
            } else if (pe.assembler.hasTexels()) {
                ++partialPages;
            }
        }
        platformLog(LogLevel::Warning, "PageDet",
                    "uniquePages=%d residentPages=%d uploadedTotal=%d "
                    "visibleCappedTiles=%d zMin=%d zMax=%d maxTileSse=%.0f "
                    "culledBySse=%d sources=%d complete=%d partial=%d "
                    "fullyResident=%d/%d worstCellRatio=%.2f",
                    lastVisiblePageCount_, pool_.residentCount(),
                    uploadedLayerTotal_, visibleCappedTiles, logZMin, logZMax,
                    maxTileSse, culledBySse,
                    static_cast<int>(providers_.size()), completePages,
                    partialPages,
                    fullyResidentTiles_, residencyCheckedTiles_,
                    static_cast<double>(worstResidentRatio_));
    }
}

TerrainPageStore::~TerrainPageStore() {
    for (auto& [pageKey, pe] : pages_) {
        for (CancellationToken& token : pe.fetchTokens) {
            token.cancel();  // 尽力取消在途(回调仍安全:只写 shared inbox)
        }
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
    // 合批 Step 2:间接纹理共享 array(固定 64² 每层,NEAREST 语义由片元
    // texelFetch/read 整数寻址保证,滤波参数仅防御)。
    TextureDesc indirDesc;
    indirDesc.width = kIndirSideTexels;
    indirDesc.height = kIndirSideTexels;
    indirDesc.arrayLayers = kIndirArrayLayers;
    indirDesc.format = TextureDesc::Format::RGBA8;
    indirDesc.mipmap = false;
    indirDesc.minFilter = TextureDesc::Filter::Nearest;
    indirDesc.magFilter = TextureDesc::Filter::Nearest;
    indirDesc.wrapS = TextureDesc::Wrap::Clamp;
    indirDesc.wrapT = TextureDesc::Wrap::Clamp;
    indirArrayTexture_ = device_->createTexture(indirDesc);
    if (!indirArrayTexture_) {
        arrayTexture_.reset();
        device_ = nullptr;
        return false;
    }
    pool_.configure(config_.maxPages, /*blockLayers=*/1);
    indirPool_.configure(kIndirArrayLayers, /*blockLayers=*/1);
    inbox_ = std::make_shared<PendingInbox>();
    return true;
}

void TerrainPageStore::erasePageEntry(uint64_t pageKey) {
    const auto it = pages_.find(pageKey);
    if (it == pages_.end()) {
        return;
    }
    for (CancellationToken& token : it->second.fetchTokens) {
        token.cancel();  // 在途 fetch 全部作废(到达也会被 drain 校验丢弃)
    }
    pages_.erase(it);
}

void TerrainPageStore::applyToTerrainCommand(RenderCommand& cmd,
                                             const TilesetTile& tile) {
    if (!arrayTexture_ || !cmd.terrainRenderContent) {
        return;
    }
    // 只挂真实地形(fill/ellipsoid proxy 不是最终高清目标 → 留 mappedRaster)。
    if (cmd.terrainSurfaceSource != TerrainSurfaceCommandSource::RealTerrain) {
        return;
    }
    // C-1:源列表由 determination 每帧刷新(它是唯一事实源,与 mappedRaster 同序),
    // 此处不再兜底捕获 —— 两处各自捕获正是「靠后 overlay 被静默丢弃」的温床。

    // B2b:无相机,只 bind determination 本帧建好的稀疏间接纹理。无 TileIndir
    // (未 determined / 无可见页 / 层被夺)→ 不动 → mappedRaster(决策② 共存,
    // 零回归)。合批 Step 2:间接纹理 = 共享 array + 层号(u_terrainLayers.y)。
    const auto it = tileIndirs_.find(packKey(tile.key));
    if (it == tileIndirs_.end() || it->second.layer < 0) {
        return;
    }
    const TileIndir& ind = it->second;
    if (cmd.textures.size() <=
        static_cast<size_t>(kGltfPageStoreIndirTextureSlot)) {
        cmd.textures.resize(
            static_cast<size_t>(kGltfPageStoreIndirTextureSlot) + 1u, nullptr);
    }
    cmd.textures[kGltfPageStoreArrayTextureSlot] = arrayTexture_.get();
    // 间接纹理 array 绑 slot21,片元经层号 fetch 定位 layer + 读 A 通道作 miss
    // 回退 factor。
    cmd.textures[kGltfPageStoreIndirTextureSlot] = indirArrayTexture_.get();
    // enabled=1、gridN → 片元采页存储。layer 由间接纹理 RG 承载,resident/miss 由
    // 其 A 承载;pageStoreParams.z/.w 不再用(留 0)。
    cmd.gltfUniforms.pageStoreParams = {1.0f, static_cast<float>(ind.gridN),
                                        0.0f, 0.0f};
    cmd.gltfUniforms.terrainLayers[1] = static_cast<float>(ind.layer);
    cmd.terrainPageStoreFullyResident = ind.fullyResident;
}

void TerrainPageStore::tick() {
    if (!arrayTexture_) {
        return;
    }
    ++frameId_;  // 推进帧号(下帧 determination 的 LRU touch/淘汰基准)
    if (decorator_) {
        decorator_->tickDecorator();  // 先让叠画方把网格传上 GPU
        decoratorTickedFrame_ = frameId_;
    }
    drainInbox();  // fetch 已在 determination 页首次命中时 kick
    retryPendingDecorations();
}

void TerrainPageStore::retryPendingDecorations() {
    if (!decorator_ || !arrayTexture_) {
        return;
    }
    // 每帧有上限:叠画是真 draw,不能跟主 pass 抢预算。未轮到的页下帧继续
    // (页存储自己迭代 → 被 LRU 换租的页自然不在表里,不会画进别人的层)。
    int budget = config_.maxUploadsPerFrame;
    for (auto& [pageKey, pe] : pages_) {
        if (budget <= 0) {
            break;
        }
        if (pe.decorated || !pe.assembler.hasTexels() || pe.layer < 0) {
            continue;
        }
        --budget;
        GE_CONTRACT(contracts::Id::PageDecorateOrdering,
                    decoratorTickedFrame_ == frameId_,
                    "path=retryPending frame=%llu tickedFrame=%llu layer=%d",
                    (unsigned long long)frameId_,
                    (unsigned long long)decoratorTickedFrame_,
                    pe.layer);
        pe.decorated = decorator_->decoratePage(unpackKey(pageKey),
                                                arrayTexture_.get(), pe.layer);
    }
}

void TerrainPageStore::kickPageFetches(const TileKey& pageTileKey,
                                       uint64_t pageKey, int layer,
                                       PageEntry& entry) {
    entry.fetchTokens.assign(providers_.size(), CancellationToken{});
    std::shared_ptr<PendingInbox> inbox = inbox_;
    for (size_t s = 0; s < providers_.size(); ++s) {
        if (providers_[s] == nullptr) {
            continue;
        }
        const int source = static_cast<int>(s);
        // C-1b:**每个源各自钳到自己的 maxZoom**。页 zoom 由屏幕(与底图上限)驱动,
        // 常深于标注/矢量类源的上限;不钳则那些源恒 404 → 永不到达 → assembler 卡在
        // 前序、该源在页内彻底消失(真机踩过:矢量路网整片没了)。钳到祖先后按
        // scale-bias 取子矩形放大 —— 与 mappedRaster 那条路逐瓦片挑祖先同语义。
        TileKey fetchKey = pageTileKey;
        int depth = pageTileKey.z - providers_[s]->getMaximumLevel();
        depth = std::max(0, std::min(depth, pageTileKey.z));
        int subX = 0;
        int subY = 0;
        if (depth > 0) {
            subX = pageTileKey.x & ((1 << depth) - 1);
            subY = pageTileKey.y & ((1 << depth) - 1);
            fetchKey.z = pageTileKey.z - depth;
            fetchKey.x = pageTileKey.x >> depth;
            fetchKey.y = pageTileKey.y >> depth;
        }
        ImageryProvider& imagery = providers_[s]->getImageryProvider();
        imagery.requestTile(
            fetchKey, entry.fetchTokens[s],
            [inbox, pageKey, layer, source, depth, subX, subY](
                const TileKey&, std::unique_ptr<DecodedImage> image) {
                if (!image) return;
                std::lock_guard<std::mutex> lock(inbox->mutex);
                inbox->pages.push_back(
                    {pageKey, layer, source, depth, subX, subY, std::move(image)});
            });
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
        // C-1b:**不再要求源尺寸等于页边长**。resamplePageSource 按 image 自身
        // 尺寸重采样,任意 tileSize 的 provider 都能进页。旧护栏「非 256² 跳过」
        // 是静默丢弃 —— 真机踩过:矢量源 tileSize=512,图非空(故不打 NULL 日志)
        // 却恒被丢,表现为该源在页内完全不存在,且没有任何一条错误日志。
        if (!image || image->width <= 0 || image->height <= 0 ||
            image->pixels.empty()) {
            continue;
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
        resamplePageSource(*image, item.ancestorDepth, item.subX, item.subY,
                           side, rgba);
        // C-1:按源序合成进 assembler;只有真的推进了合成才上传(乱序早到的源
        // 先暂存不上传,重复到达幂等丢弃 → 不浪费本帧上传预算)。
        if (!pe.assembler.accept(item.source, rgba.data())) {
            continue;
        }
        device_->updateTextureRegion(arrayTexture_.get(), 0, 0, side, side,
                                     pe.assembler.texels().data(),
                                     static_cast<size_t>(side) * 4u,
                                     item.layer);
        // hasTexels 已为真 → 下帧 determination 重建 indir 时该 cell 变 resident。
        if (pe.assembler.complete()) {
            pe.assembler.releaseBuffers();  // 全源到齐:稳态零额外内存
        }
        // C-2c:本次上传覆盖了此前的 GPU 叠画 → 标记未叠画并立刻试一次。
        // 未就绪(源瓦片在路上)时由 retryPendingDecorations 后续帧接着试。
        pe.decorated = false;
        if (decorator_) {
            GE_CONTRACT(contracts::Id::PageDecorateOrdering,
                        decoratorTickedFrame_ == frameId_,
                        "path=drainInbox frame=%llu tickedFrame=%llu layer=%d",
                        (unsigned long long)frameId_,
                        (unsigned long long)decoratorTickedFrame_,
                        item.layer);
            pe.decorated = decorator_->decoratePage(unpackKey(item.key),
                                                    arrayTexture_.get(),
                                                    item.layer);
        }
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
