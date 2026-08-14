#include "TerrainPageStore.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

#include "../core/async/AsyncSystem.h"
#include "../core/geodesy/Gcj02CoordinateTransform.h"
#include "../core/geodesy/WebMercatorProjection.h"
#include "../tiling/RasterOverlayProjection.h"
#include "../core/math/OrientedBoundingBox.h"
#include "../debug/Contracts.h"
#include "../debug/PerfTimer.h"
#include "../debug/Policies.h"
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

/// worker 侧合成状态。mutex 序列化同页多源的 accept(alphaOver 不可交换,
/// stash 兜乱序,故任务执行次序无关紧要);cancelled 由淘汰路径置位,在途
/// 任务据此早退(迟到快照仍会被 drainReadyUploads 的账本校验丢弃 ——
/// cancelled 是省功,校验才是安全线)。
struct TerrainPageStore::PageComposeState {
    std::mutex mutex;
    PageSourceAssembler assembler;
    std::atomic<bool> cancelled{false};
};

/// worker 合成完毕的页快照投递箱(shared_ptr 持有,worker 任务可比页存储
/// 活得久)。composeMicros 是 worker 侧合成耗时累计 —— tick60 的 compose 列
/// 自此计的是 worker 时间,渲染线程成本只剩 upload/decorate 两列。
struct TerrainPageStore::ReadyUploadInbox {
    struct Item {
        uint64_t key = 0;
        int layer = 0;
        int composedSources = 0;           // 快照时的合成进度(源数)
        std::vector<uint8_t> texels;       // side²×4 快照
    };
    std::mutex mutex;
    std::vector<Item> items;
    std::atomic<uint64_t> composeMicros{0};
};

/// 刀2 场平面投递箱(shared_ptr:场生产者回调可比页存储活得久)。
/// 只投递**真场**;层复用时旧租户残留由间接纹理 A 三态门控挡住(场未 ready
/// 的层 A=128,片元不采场),不再靠占位清场(占位在同 key 页 churn 重建时会
/// 清掉刚上传的正确真场 → 运动期路网闪烁,已改用 fieldLayerKey_ 门控)。
struct TerrainPageStore::RoadFieldInbox {
    struct Item {
        uint64_t key = 0;
        int layer = 0;
        std::vector<uint8_t> r8;   // side²×1
    };
    std::mutex mutex;
    std::vector<Item> items;
};

bool TerrainPageStore::fieldLayerReady(int layer, uint64_t pageKey) const {
    if (!fieldArrayTexture_) return true;  // 无场功能 → 二态零回归
    return layer >= 0 && layer < static_cast<int>(fieldLayerKey_.size()) &&
           fieldLayerKey_[layer] == pageKey;
}

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
    // 行序原样上传:DecodedImage row 0=北,terrain 用 NW 约定(v=0 北,
    // rewriteProjectionTexCoords),两端一致故不翻转即对齐。
    for (int row = 0; row < side; ++row) {
        const float v = v0 + (static_cast<float>(row) + 0.5f) /
                                 static_cast<float>(side) * span;
        const float fy = v * static_cast<float>(ih) - 0.5f;
        uint8_t* d = out.data() + static_cast<size_t>(row) * side * 4;
        for (int col = 0; col < side; ++col) {
            const float u = u0 + (static_cast<float>(col) + 0.5f) /
                                     static_cast<float>(side) * span;
            sample(u * static_cast<float>(iw) - 0.5f, fy, d);
            d += 4;
        }
    }
}

void TerrainPageStore::encodeLayerRGBA8(int layer, bool resident,
                                        bool fieldReady, int depth,
                                        uint8_t out[4]) {
    const unsigned int v = static_cast<unsigned int>(std::max(0, layer));
    out[0] = static_cast<uint8_t>(v & 0xFFu);          // R = layer & 0xFF
    out[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);   // G = (layer >> 8) & 0xFF
    // B = per-cell 渐变 LOD 深度 d(§16.3),clamp [0, kMaxDetDepthLevels]。片元
    // span=2^d 定位粗祖先页子区;d=0 逐字节=现状精页。
    out[2] = static_cast<uint8_t>(std::clamp(depth, 0, kMaxDetDepthLevels));
    // A 三态门控(刀2 场 resident 独立):miss=0(保留 mappedRaster)、影像 resident
    // 但场未 ready=128(片元采影像不采场)、影像+场都 ready=255。片元对影像做
    // step(0.3) 离散(A≥128 → 采影像),场 gate 用 A>0.6(仅 255)。无场功能时
    // fieldReady 恒 true → 退化 0/255 二态,逐位=改造前。
    out[3] = resident ? (fieldReady ? 255 : 128) : 0;
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

TerrainPageStore::SourceTilePlacement
TerrainPageStore::placeTileInSourceGrid(
    const TileScheme& scheme,
    const Rectangle& detailsRect,
    RasterOverlayProjection projection,
    int sourceZoom,
    int gridN) {
    SourceTilePlacement placement;
    const int safeGridN = std::max(1, gridN);
    const int tilesX = std::max(1, scheme.tileCountX(sourceZoom));
    const int tilesY = std::max(1, scheme.tileCountY(sourceZoom));

    // detailsRect 在 overlay 采样空间(GCJ 时是 gcj-mercator);源瓦片编址用的是
    // 源经纬,故先退回源经纬再问 scheme 要 key。
    const Rectangle sourceRect =
        unprojectRasterSourceRectangle(detailsRect, projection);
    if (sourceRect.isEmpty() || !(sourceRect.width() > 0.0) ||
        !(sourceRect.computeHeight() > 0.0)) {
        placement.cellsX = safeGridN;
        placement.cellsY = safeGridN;
        placement.spanU = static_cast<double>(safeGridN);
        placement.spanV = static_cast<double>(safeGridN);
        return placement;
    }

    // 边界瓦片:east/north 恰落在网格线上时 positionToTile 会返回**下一格**,
    // 那一格并不被覆盖,却会让 cellsX/cellsY 多出一列一行(多取一圈源瓦片)。
    // 往内缩一个极小量把闭区间问成半开区间。
    //
    // 内缩量必须按**源瓦片**尺度取,不能按 detailsRect 尺度(如 width/gridN 的几分
    // 之一)—— GCJ 平移只有零点几格,足够大的内缩会把真实越界一并抹掉,覆盖范围
    // 少算一列 → 屏幕整片错开一整张源瓦片。这条被 UvMapsIntoCoveringRange 抓到过。
    const TileKey midKey = scheme.positionToTile(
        0.5 * (sourceRect.west() + sourceRect.east()),
        0.5 * (sourceRect.south() + sourceRect.north()),
        sourceZoom);
    const Rectangle midRect = scheme.tileToRectangle(midKey);
    const double insetX = 1e-9 * std::abs(midRect.width());
    const double insetY = 1e-9 * std::abs(midRect.computeHeight());
    const TileKey nw = scheme.positionToTile(
        sourceRect.west() + insetX,
        sourceRect.north() - insetY,
        sourceZoom);
    const TileKey se = scheme.positionToTile(
        sourceRect.east() - insetX,
        sourceRect.south() + insetY,
        sourceZoom);

    placement.x0 = std::min(nw.x, se.x);
    placement.y0 = std::min(nw.y, se.y);
    const int x1 = std::max(nw.x, se.x);
    const int y1 = std::max(nw.y, se.y);
    placement.x0 = std::clamp(placement.x0, 0, tilesX - 1);
    placement.y0 = std::clamp(placement.y0, 0, tilesY - 1);
    placement.cellsX =
        std::clamp(x1, 0, tilesX - 1) - placement.x0 + 1;
    placement.cellsY =
        std::clamp(y1, 0, tilesY - 1) - placement.y0 + 1;

    // 覆盖范围在 overlay 采样空间的矩形 = 两个角瓦片矩形的并,再投影。逐字段走
    // scheme 而不是用 levelResolution 自己乘,是为了让极区/非均匀方案也成立。
    TileKey cornerNw{scheme.id(), sourceZoom, placement.x0, placement.y0};
    TileKey cornerSe{scheme.id(), sourceZoom,
                     placement.x0 + placement.cellsX - 1,
                     placement.y0 + placement.cellsY - 1};
    const Rectangle rectNw = projectRasterSourceRectangle(
        scheme.tileToRectangle(cornerNw), projection);
    const Rectangle rectSe = projectRasterSourceRectangle(
        scheme.tileToRectangle(cornerSe), projection);
    const double rangeWest = std::min(rectNw.west(), rectSe.west());
    const double rangeEast = std::max(rectNw.east(), rectSe.east());
    const double rangeSouth = std::min(rectNw.south(), rectSe.south());
    const double rangeNorth = std::max(rectNw.north(), rectSe.north());
    const double rangeWidth = rangeEast - rangeWest;
    const double rangeHeight = rangeNorth - rangeSouth;
    if (!(rangeWidth > 0.0) || !(rangeHeight > 0.0)) {
        placement.cellsX = safeGridN;
        placement.cellsY = safeGridN;
        placement.spanU = static_cast<double>(safeGridN);
        placement.spanV = static_cast<double>(safeGridN);
        return placement;
    }

    // 单位换成「源瓦片」:片元 t = origin + uv*span → floor 即 cell 下标。
    // V 走 NW 约定(v=0 在北),与 resamplePageSource/顶点烘焙两端一致。
    const double cellsXd = static_cast<double>(placement.cellsX);
    const double cellsYd = static_cast<double>(placement.cellsY);
    placement.originU =
        (detailsRect.west() - rangeWest) / rangeWidth * cellsXd;
    placement.spanU = detailsRect.width() / rangeWidth * cellsXd;
    placement.originV =
        (rangeNorth - detailsRect.north()) / rangeHeight * cellsYd;
    placement.spanV =
        detailsRect.computeHeight() / rangeHeight * cellsYd;
    return placement;
}

void TerrainPageStore::computeGeomAffine(const TileScheme& scheme,
                                         RasterOverlayProjection projection,
                                         const TileKey& key,
                                         int sourceZoom,
                                         int baseX,
                                         int baseY,
                                         float out[6]) {
    // 源格在 overlay 采样空间(mercator)均匀分布(placeTileInSourceGrid 的
    // range 映射即按此),故连续格坐标 = 全幅 mercator 线性。角点经
    // projectWorldPositionForRasterOverlay 投影 —— 与逐顶点 texcoord 同入口,
    // GCJ 逐点 fromWgs84,不做整瓦近似。
    const Rectangle full =
        WebMercatorProjection::computeMaximumProjectedRectangle(
            Ellipsoid::WGS84());
    const double tilesX =
        static_cast<double>(std::max(1, scheme.tileCountX(sourceZoom)));
    const double tilesY =
        static_cast<double>(std::max(1, scheme.tileCountY(sourceZoom)));
    const Rectangle rect = scheme.tileToRectangle(key);
    const double fullWidth = full.width();
    const double fullHeight = full.computeHeight();
    const auto grid = [&](double lng, double lat, double& gx, double& gy) {
        const Vec3 m = projectWorldPositionForRasterOverlay(
            Cartographic::fromRadians(lng, lat), projection);
        gx = (m.x() - full.west()) / fullWidth * tilesX -
             static_cast<double>(baseX);
        gy = (full.north() - m.y()) / fullHeight * tilesY -
             static_cast<double>(baseY);  // NW 约定:y 随南增
    };
    double nwX = 0.0, nwY = 0.0, neX = 0.0, neY = 0.0, swX = 0.0, swY = 0.0;
    grid(rect.west(), rect.north(), nwX, nwY);
    grid(rect.east(), rect.north(), neX, neY);
    grid(rect.west(), rect.south(), swX, swY);
    // 扭曲项(第 4 角独立度)~2cm,弃;交叉项保留(沿边线性变化 ~30m 是主项)。
    out[0] = static_cast<float>(nwX);
    out[1] = static_cast<float>(nwY);
    out[2] = static_cast<float>(neX - nwX);
    out[3] = static_cast<float>(neY - nwY);
    out[4] = static_cast<float>(swX - nwX);
    out[5] = static_cast<float>(swY - nwY);
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
    // 放在最前而不是最后:本函数有多处早退,放末尾会被跳过。用上一帧的在途
    // 状态对账 —— 迟一帧只会多渲一帧(安全方向),而漏对账是永远停不下来。
    syncWorkTicket();
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
            entry.fieldToken.cancel();
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

        int gridN = subtileGridN(tile->key.z, zoom);
        const double minH = TileBoundsMetrics::terrainMinimumHeight(*tile);
        const double maxH = TileBoundsMetrics::terrainMaximumHeight(*tile);
        const uint64_t tileKeyPacked = packKey(tile->key);
        // 影像 fetch schemeId(terrain tileKey.schemeId 可能异号)由影像 scheme 取。
        const Rectangle tileRect = scheme.tileToRectangle(tile->key);
        const double cLng = 0.5 * (tileRect.west() + tileRect.east());
        const double cLat = 0.5 * (tileRect.south() + tileRect.north());
        const SchemeId imgSchemeId =
            scheme.positionToTile(cLng, cLat, tile->key.z).schemeId;

        // cell 网格落位:必须用 **details 里的**矩形与 texcoord 下标,不能就地
        // 重算 —— details 可能来自模型真实包围(见 TileRasterOverlayDetailsGenerator)
        // 与瓦片声明矩形不等,而逐顶点 UV 正是按 details 归一化的,重算会让 UV 与
        // cell 网格系统性错开。拿不到 details(尚未生成)时退回几何等分 = 现状。
        const RasterOverlayProjection projection = provider->getProjection();
        const RasterOverlayDetails& details =
            tile->content.renderContent.rasterOverlayDetails();
        const Rectangle* detailsRect =
            details.findRectangleForOverlayProjection(projection);
        const int texCoordSet =
            details.textureCoordinateIDForProjection(projection);
        SourceTilePlacement placement;
        placement.x0 = tile->key.x * gridN;
        placement.y0 = tile->key.y * gridN;
        placement.cellsX = gridN;
        placement.cellsY = gridN;
        placement.spanU = static_cast<double>(gridN);
        placement.spanV = static_cast<double>(gridN);
        if (detailsRect != nullptr && texCoordSet >= 0) {
            placement = placeTileInSourceGrid(
                scheme, *detailsRect, projection, zoom, gridN);
            // 源网格不对齐时覆盖范围会多一列一行,而间接纹理边长固定
            // kIndirSideTexels(=64)且 gridN 上限恰好也是 64 —— 不设闸就正好在最深
            // 一级越界写。降一级 zoom 重算(只损失最深那一档的清晰度)。
            if (placement.cellsX > kIndirSideTexels ||
                placement.cellsY > kIndirSideTexels) {
                zoom = std::max(tile->key.z, zoom - 1);
                gridN = subtileGridN(tile->key.z, zoom);
                placement = placeTileInSourceGrid(
                    scheme, *detailsRect, projection, zoom, gridN);
                placement.cellsX =
                    std::min(placement.cellsX, kIndirSideTexels);
                placement.cellsY =
                    std::min(placement.cellsY, kIndirSideTexels);
            }
        }

        // 源坐标 → 世界坐标的平移量。standard 时 fromWgs84 是恒等 → 恒 0。
        double worldOffsetLng = 0.0;
        double worldOffsetLat = 0.0;
        bool crossesGcjBoundary = false;
        if (projection == RasterOverlayProjection::Gcj02WebMercator) {
            const Cartographic center =
                Cartographic::fromRadians(cLng, cLat);
            const Cartographic shifted =
                Gcj02CoordinateTransform::fromWgs84(center);
            worldOffsetLng = shifted.longitude() - center.longitude();
            worldOffsetLat = shifted.latitude() - center.latitude();
            crossesGcjBoundary =
                Gcj02CoordinateTransform::crossesChinaBounds(tileRect);
        }

        detParamsScratch_.push_back(
            {tile, tileKeyPacked, zoom, gridN, minH, maxH, imgSchemeId,
             placement, texCoordSet >= 0 ? texCoordSet : 0,
             worldOffsetLng, worldOffsetLat, crossesGcjBoundary});
    }

    // ===== Phase 2:逐瓦片 —— geom 缓存(相机无关几何,placement 变才重建)+ 每帧分类
    // (frustum/SSE)。驻留编码每帧必跑,故异步到页逐帧点亮无 stale。=====
    visiblePagesScratch_.clear();
    int zMin = std::numeric_limits<int>::max();
    int zMax = std::numeric_limits<int>::min();
    // 分类每帧必跑 → coarseFarCells 每帧从 0 重累加(全瓦合计,供 PageDet 日志)。
    int coarseFarCells = 0;
    const int visibleCappedTiles = static_cast<int>(detParamsScratch_.size());

    for (const DetTileParam& p : detParamsScratch_) {
        DetTileCacheEntry& cache = detTileCache_[p.tileKeyPacked];
        // geom 缓存:相机无关的 per-cell 几何(源矩形→OBB,建构含三角函数,是几何
        // walk 的大头)。只在 placement/几何变化时重建 —— 平滑运动下 placement 稳定 →
        // 跨帧复用,免每帧重算 OBB。geom 逐位等于现算,故下面的分类结果不变(无白面/
        // 漏底)。此前这里靠 bit-exact 视图签名整体跳过 walk,运动下恒判脏=每帧全量
        // 重跑;拆成「几何缓存 + 每帧分类」后运动稳态 walk 6-12ms → ~0.3ms。
        if (cache.gridN != p.gridN ||
            cache.cellsX != p.placement.cellsX ||
            cache.cellsY != p.placement.cellsY ||
            cache.cellX0 != p.placement.x0 ||
            cache.cellY0 != p.placement.y0) {
            cache.gridN = p.gridN;
            cache.cellsX = p.placement.cellsX;
            cache.cellsY = p.placement.cellsY;
            cache.cellX0 = p.placement.x0;
            cache.cellY0 = p.placement.y0;
            cache.geom.clear();
            // cell 网格 = **源瓦片网格**(D)。标准 overlay 下 placement 退化成
            // x0=key.x*gridN、cells=gridN,与改造前逐格相同;GCJ 等源网格不对齐的
            // overlay 才走到偏移后的 range(通常多一列一行)。
            for (int dy = 0; dy < p.placement.cellsY; ++dy) {
                for (int dx = 0; dx < p.placement.cellsX; ++dx) {
                    TileKey sub;
                    sub.schemeId = p.tile->key.schemeId;  // 视锥/rect 用 terrain scheme
                    sub.z = p.zoom;
                    sub.x = p.placement.x0 + dx;
                    sub.y = p.placement.y0 + dy;
                    // 覆盖范围可能比瓦片宽一格:完全不压在本瓦片上的 cell 不产生
                    // 片元,不入 geom。标准 overlay 下恒相交 → 逐格等价于改造前。
                    const double cellLo = static_cast<double>(dx);
                    const double cellHi = cellLo + 1.0;
                    const double tileLo = p.placement.originU;
                    const double tileHi = tileLo + p.placement.spanU;
                    const double cellLoV = static_cast<double>(dy);
                    const double cellHiV = cellLoV + 1.0;
                    const double tileLoV = p.placement.originV;
                    const double tileHiV = tileLoV + p.placement.spanV;
                    if (std::min(cellHi, tileHi) <= std::max(cellLo, tileLo) ||
                        std::min(cellHiV, tileHiV) <=
                            std::max(cellLoV, tileLoV)) {
                        continue;
                    }
                    // 编址在源空间,剔除/测距在世界空间 —— 见 DetTileParam
                    // 的 worldOffset 注释(混用会在视锥边缘误剔出一片空洞)。
                    const Rectangle sourceRect = scheme.tileToRectangle(sub);
                    const Rectangle subRect =
                        (p.worldOffsetLng == 0.0 && p.worldOffsetLat == 0.0)
                            ? sourceRect
                            : Rectangle(
                                  sourceRect.west() - p.worldOffsetLng,
                                  sourceRect.south() - p.worldOffsetLat,
                                  sourceRect.east() - p.worldOffsetLng,
                                  sourceRect.north() - p.worldOffsetLat);
                    const std::optional<OrientedBoundingBox> obb =
                        TileBoundsMetrics::boundingRegionObb(subRect, p.minH,
                                                             p.maxH);
                    if (!obb) {
                        continue;  // 无包围体 → 无从测距(编码时默认 miss)
                    }
                    cache.geom.emplace_back(dx, dy, sub.x, sub.y, *obb);
                }
            }
        }

        // [pageStore第一刀] 分类(相机相关,每帧必跑):在缓存几何上做 frustum+SSE →
        // kept。逐位复刻原 walk 的相机相关段,只是 OBB/矩形不再每帧重建。
        cache.kept.clear();
        cache.coarseFarCells = 0;
        for (const DetCellGeom& g : cache.geom) {
            // 跨 GCJ 边界的瓦片放弃视锥剔除:worldOffset 是阶跃近似,假的"视锥外"
            // 会渲成纯白面(见 DetTileParam 的 crossesGcjBoundary 注释)。保守全收。
            if (!p.crossesGcjBoundary &&
                !view.frustum.intersectsOBB(g.obb)) {
                continue;  // 视锥外 → 不入 kept(编码时默认 miss)
            }
            // per-cell 渐变 LOD(§16.3):用**瓦片级** geomError 在 cell 距离处的屏幕
            // 误差 → cell 该细化到的影像 zoom Za。近 cell→Za=Z、d=0(逐字节=现状精
            // 页);远 cell→Za 渐降采粗祖先页(池去重,工作集有界)。
            const double cellDist = std::sqrt(
                g.obb.computeDistanceSquaredToPosition(view.position));
            const double cellSse =
                TileSelectionInputMetrics::screenSpaceErrorForView(
                    p.tile->geometricError, view.projectionMatrix,
                    view.viewportHeightPixels, cellDist);
            // [远景矢量补覆盖] §16.3⑥ 曾把屏幕贡献 < 1/4 阈值的 cell 判「真 miss」直接
            // 剔除(A=0)回落 mappedRaster。「页=纯影像」时代无害(远景卫图 mappedRaster
            // 兜底够用),但刀1/刀2 后 drape 面与 SDF 路网场**只寄生在页上**、mappedRaster
            // 无矢量等价物 → 远景高俯角整片没水面/没路网。改法:不再 continue 丢弃,而是让
            // 它照常走下面的渐变路径取**最粗祖先页**(cellSse<threshold → za 恒钳到 tileZ)。
            //
            // 边界(为何不重现注释担心的「near-nadir 枚举爆量」):za 钳到 tileZ 后,本 cell
            // 的祖先 pageKey = (tileZ,tileX,tileY),与本瓦**所有** za=tileZ 的近地板 cell 同
            // 一张页 —— 补覆盖只是让更多 cell 指向渐变**已经请求**的那张粗页,**零新增页**,
            // 工作集不变。地板当年真正防的是 §15.3 硬剔可能落到的**细**页;对「远 cell→共享
            // 粗页」这条路径,爆量从来不成立,地板只是白白丢了远景矢量、无页预算收益。
            //
            // 计数保留仅作真机验收诊断(远景该 >0 = 补覆盖生效),不再据此剔除或卡合批。
            if (cellSse < threshold * kCellSseMissFloorFraction) {
                ++coarseFarCells;
                ++cache.coarseFarCells;  // per-tile 诊断:降级到粗页兜底的远 cell 数
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
            kc.dx = g.dx;
            kc.dy = g.dy;
            kc.d = d;
            const int cx = g.subX >> d;
            const int cy = g.subY >> d;
            kc.fetchKey.schemeId = p.imgSchemeId;
            kc.fetchKey.z = za;
            kc.fetchKey.x = cx;
            kc.fetchKey.y = cy;
            kc.pageKey = packKey(kc.fetchKey);  // 粗页去重 key(同祖先→同 key)
            cache.kept.push_back(kc);
        }

        // [拉远连续性] 整瓦「锚页」恒驻(za=tileZ,最粗层):祖先 walk 的结构约束
        // 是只能从 p.zoom 走到 tileZ、比 p.zoom 细的页不可寻址(一粗 cell 需 4 细页)。
        // 拉远时 za 逐层变粗、新层页 just-in-time 才 kick,p.zoom 掉穿"最后一层暖层"
        // 的瞬间 walk 全空 → A=0:影像有 mappedRaster 兜底看不出断,线没有等价物 →
        // 整片消失再出现。锚页让最粗一层永远有双就绪兜底:任意拉远速度,线最多短暂
        // 变粗(锚页干线级),不消失;细层页到达逐层锐化。成本 +1 页/可见瓦(≤~52,
        // 池 512 富余);与远景补覆盖的地板 cell 目标页同 key,try_emplace 天然去重。
        // up=0 本瓦锚页;up=1 **父瓦锚页预暖**——换代(子→父)瞬间父瓦 p.zoom 收缩到
        // ≈tileZ、gridN 收到 1-2,单 cell 直接要父瓦整页,而它在换代帧才首次 kick(冷);
        // 子瓦时期的细锚页比新 p.zoom 细、结构上不可寻址(d≥0)→ walk 全空 → 整屏线
        // 同步消失(平移只在屏缘小面积异步补,故只有缩放显)。子瓦还在渲染时就把父锚
        // 页烘好,换代帧 Tier1 直接命中。4 子共享 1 父锚,增量 ≈ +25% 瓦数。
        for (int up = 0; up <= 1 && p.tile->key.z - up >= 0; ++up) {
            TileKey anchorKey;
            anchorKey.schemeId = p.imgSchemeId;
            anchorKey.z = p.tile->key.z - up;
            anchorKey.x = p.tile->key.x >> up;
            anchorKey.y = p.tile->key.y >> up;
            const uint64_t anchorPageKey = packKey(anchorKey);
            uint64_t anchorEvicted = 0;
            const int anchorLayer =
                pool_.acquire(anchorPageKey, frameId_, &anchorEvicted);
            if (anchorEvicted != 0) {
                erasePageEntry(anchorEvicted);
            }
            if (anchorLayer >= 0) {
                auto [it, inserted] = pages_.try_emplace(anchorPageKey);
                PageEntry& pe = it->second;
                if (inserted) {
                    pe.layer = anchorLayer;
                    pe.compose = std::make_shared<PageComposeState>();
                    pe.compose->assembler.configure(
                        static_cast<int>(providers_.size()),
                        config_.pageSizeTexels);
                    pe.totalSources = static_cast<int>(providers_.size());
                    pe.lastProgressFrame = frameId_;
                    kickPageFetches(anchorKey, anchorPageKey, anchorLayer, pe);
                }
            }
        }

        // 驻留编码(**每帧必跑**):按当前 resident/uploaded 重建间接纹理。
        // residentCells 统计本帧真正拿到高清页(A=255,含祖先回退)的 cell 数;
        // == gridN² 即「全 cell 驻留」= 合批资格闸(此时 mappedRaster fallback
        // 必不被采样 → 批命令可丢 mappedRaster,见 TerrainInstanceBatcher)。
        indirTexelsScratch_.assign(
            static_cast<size_t>(p.placement.cellsX) *
                static_cast<size_t>(p.placement.cellsY) * 4u,
            0);
        int residentCells = 0;
        for (const DetKeptCell& kc : cache.kept) {
            uint8_t* texel =
                indirTexelsScratch_.data() +
                (static_cast<size_t>(kc.dy) * p.placement.cellsX + kc.dx) * 4u;
            visiblePagesScratch_.insert(kc.pageKey);  // 去重计数(粗页共享 → 少)
            zMin = std::min(zMin, kc.fetchKey.z);  // 实际页 zoom = Za(渐变后 ≤ Z)
            zMax = std::max(zMax, kc.fetchKey.z);
            // 请求目标页(za,d)并占槽/touch(建页副作用:emplace + kick fetch)。
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
                    pe.compose = std::make_shared<PageComposeState>();
                    pe.compose->assembler.configure(
                        static_cast<int>(providers_.size()),
                        config_.pageSizeTexels);
                    pe.totalSources = static_cast<int>(providers_.size());
                    pe.lastProgressFrame = frameId_;  // 建页即算一次进度
                    kickPageFetches(kc.fetchKey, kc.pageKey, layer, pe);
                }
            }
            // [刀2 运动闪烁根修] 沿祖先链一次 walk(ad 升序=细→粗;深度轴统一 =
            // p.zoom-pageZoom,self 恰在 ad=kc.d),分三层择页:
            //   Tier1 最细**双就绪**页(影像+场都在)→ 线稳、面/线 LOD 同步升级(消闪)
            //   Tier2 最细**影像就绪**页(场 pending)→ A=128:面走快路、线短暂缺
            //         (=改造前"抗模糊带"行为;救面的影像就绪页不再被场 gate 误杀成 A=0)
            //   Tier3 全冷 → A=0 回落 mappedRaster
            // 为何 Tier1 宁取更粗的双就绪页:线纯 MVT 派生、无卫星快源,细页场必然滞后
            // 卫星;若一见细页卫星就切过去会 A=128 掉线(旧行为=闪)。场未就绪时继续用已
            // 双就绪的粗祖先(面粗一级、几十 ms、无感),细页场落地下帧升级 → 线不闪。
            // 关键:找不到双就绪祖先时**退回 Tier2 影像就绪**(而非 A=0),故最坏=改造前,
            // 不出现"线消失+面降级"。场功能关闭时 fieldLayerReady 恒 true → Tier1=Tier2,
            // 逐字节回到改造前。
            const int subX = p.placement.x0 + kc.dx;
            const int subY = p.placement.y0 + kc.dy;
            const int maxAd = p.zoom - p.tile->key.z;  // za=Z 到 tileZ 的最大深度
            int dualLayer = -1, dualD = -1;
            uint64_t dualKey = 0;  // 最细双就绪页(Tier1)
            int imgLayer = -1, imgD = -1;
            uint64_t imgKey = 0;  // 最细影像就绪页(Tier2 兜底)
            for (int ad = 0; ad <= maxAd; ++ad) {
                TileKey aKey;
                aKey.schemeId = p.imgSchemeId;
                aKey.z = p.zoom - ad;
                aKey.x = subX >> ad;
                aKey.y = subY >> ad;
                const uint64_t aPageKey = packKey(aKey);
                const auto ait = pages_.find(aPageKey);
                if (ait == pages_.end() || !ait->second.uploadedTexels()) {
                    continue;
                }
                if (imgLayer < 0) {  // 最细影像就绪 = 首个 uploaded(仅记一次)
                    imgLayer = ait->second.layer;
                    imgD = ad;
                    imgKey = aPageKey;
                }
                if (fieldLayerReady(ait->second.layer, aPageKey)) {
                    dualLayer = ait->second.layer;  // 最细双就绪 = 首个场也就绪
                    dualD = ad;
                    dualKey = aPageKey;
                    break;  // 更粗的无需再看
                }
            }
            if (dualLayer >= 0) {
                pool_.touch(dualKey, frameId_);  // 显示中的页不该被淘汰
                encodeLayerRGBA8(dualLayer, true, true, dualD, texel);
                ++residentCells;
            } else if (imgLayer >= 0) {
                pool_.touch(imgKey, frameId_);
                encodeLayerRGBA8(imgLayer, true, false, imgD,
                                 texel);  // A=128:面走快路,线短暂缺
                ++residentCells;
            } else {
                // 全冷 → mappedRaster(resident=false;fieldReady 无关给 true)。
                encodeLayerRGBA8(0, false, true, kc.d, texel);
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
            // 策略生效率:间接纹理层的**无换租获取率**。稳态应接近 1;持续偏低 =
            // 层池容量不足以承载当前可见集,每帧互相踢(thrash),表现为闪烁/重传。
            policy::observe(policy::Id::IndirLayerAllocNoEvict,
                            evicted == 0 ? 1 : 0, 1);
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
            // 尺寸与行距必须跟 **cell 网格**走。写成 gridN 而缓冲按 cellsX 排,
            // 行距差一格 → 每行递进错位 → 屏幕大片读到未初始化 texel(A=0)只剩
            // 零星正确块。真机截图立刻现形,host 测试看不到(不走纹理上传)。
            device_->updateTextureRegion(
                indirArrayTexture_.get(), 0, 0,
                p.placement.cellsX, p.placement.cellsY,
                indirTexelsScratch_.data(),
                static_cast<size_t>(p.placement.cellsX) * 4u, ind.layer);
        }
        ind.gridN = p.gridN;
        ind.placement = p.placement;
        ind.texCoordSet = p.texCoordSet;
        // [瓦界对齐] instanced 管线的几何 UV→源格仿射(每帧重算:zoom/placement
        // 随相机变,3 次角点投影/瓦 ≈ 微秒级,缓存守卫不值得引入失效面)。
        computeGeomAffine(scheme, provider->getProjection(), p.tile->key,
                          p.zoom, p.placement.x0, p.placement.y0,
                          ind.geomAffine);
        // 全 cell 驻留(含被 frustum/SSE 剔除的 cell 未 kept → 不计 → 达不到
        // gridN²,该瓦片留逐 draw = 保守但安全,决不丢影像)= 合批资格。
        // 合批资格 = 「**所有会产生片元的 cell 都有页**」。
        //
        // 批命令共享首实例的纹理(batch.textures = first.textures),而实例化片元
        // shader 里根本没有 mappedRaster 采样器 —— A=0 的 cell 不会回落祖先影像,
        // 而是停在 u_baseColor,渲染成被光照打亮的纯色面(真机实测:半屏纯白,
        // 地形起伏还在,像雪山,比出洞更难在截图里被认出来)。
        //
        // 所以两类 cell 要分开看:
        //   视锥外剔除   → 不产生片元 → 无所谓 → 可放行
        //   kept 但页未到 → 产生片元 → 必须挡住(residentCells 覆盖)
        // (曾有第三类「SSE 地板剔除」cell 会产生片元却无页,须单独挡;远景补覆盖后它们
        //  已进 kept、取粗页,故并入「kept 但页未到」由 residentCells 统一覆盖,不再单列。)
        //
        // 旧条件 residentCells == gridN² 把各类一起挡了,安全但在 gridN 稍大时
        // **事实上不可达**(真机 gridN=32 要 1024 cell 全驻留,而全局只有 ~52 页),
        // 合批因此长期空转。现条件保留全部安全性,同时让闸真的可达。
        ind.fullyResident = ind.layer >= 0 &&
                            residentCells == static_cast<int>(cache.kept.size());
        // 合批资格的**唯一**卡点在这里,但此前只有布尔结果、没有距离达标多远的量:
        // 于是"合批为什么一个批都不成形"完全不可查(BatchDet 只能报到
        // notFullyResident 为止)。记下最差覆盖率,把"差一点"与"根本达不到"分开。
        // 策略生效率:会产生片元的 cell 里有多少真拿到了高清页。
        // 分母 = kept(过视锥的全部 cell,含被降级到粗页的远 cell —— 远景补覆盖后它们
        // 都在 kept 里)。差额是页尚未到达的 cell,回落 mappedRaster = 糊。
        policy::observe(policy::Id::CellPageCoverage, residentCells,
                        static_cast<int>(cache.kept.size()));
        // 分母跟 **cell 网格**走,不跟几何等分走:GCJ 下二者不等(源网格多一列
        // 一行),用 gridN² 会让覆盖率恒 <1 而看不出是分母错还是真没覆盖。
        const int cellsNeeded = p.placement.cellsX * p.placement.cellsY;
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

    // 策略生效率:可见瓦片里有多少达到了合批资格(= 所有产片元的 cell 都有页)。
    // 这个比率恒 0 正是"资格闸事实上不可达"那次的表征,当时无人察觉。
    policy::observe(policy::Id::PageResidency,
                    fullyResidentTiles_, residencyCheckedTiles_);

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
            if (pe.uploadComplete()) {
                ++completePages;
            } else if (pe.uploadedTexels()) {
                ++partialPages;
            }
        }
        platformLog(LogLevel::Warning, "PageDet",
                    "uniquePages=%d residentPages=%d uploadedTotal=%d "
                    "visibleCappedTiles=%d zMin=%d zMax=%d maxTileSse=%.0f "
                    "coarseFarCells=%d sources=%d complete=%d partial=%d "
                    "fullyResident=%d/%d worstCellRatio=%.2f",
                    lastVisiblePageCount_, pool_.residentCount(),
                    uploadedLayerTotal_, visibleCappedTiles, logZMin, logZMax,
                    maxTileSse, coarseFarCells,
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
        if (pe.compose) {
            pe.compose->cancelled.store(true, std::memory_order_release);
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
    readyInbox_ = std::make_shared<ReadyUploadInbox>();
    // 刀2 场"第二平面":R8、与影像 array 同尺寸同层数(层号一一对应)。
    // 创建失败只禁用场平面(roadFieldRequest 清空),影像页不受影响。
    if (config_.roadFieldRequest) {
        TextureDesc fieldDesc = desc;
        fieldDesc.format = TextureDesc::Format::R8;
        fieldArrayTexture_ = device_->createTexture(fieldDesc);
        if (fieldArrayTexture_) {
            fieldInbox_ = std::make_shared<RoadFieldInbox>();
            // 每层真场租户 key,初值哨兵(≠任何合法 packKey)→ 首访必判 pending。
            fieldLayerKey_.assign(static_cast<size_t>(config_.maxPages),
                                  kInvalidFieldKey);
            platformLog(LogLevel::Info, "PageStore",
                        "roadField plane ready (R8 %dx%d x%d layers)",
                        config_.pageSizeTexels, config_.pageSizeTexels,
                        config_.maxPages);
        } else {
            platformLog(LogLevel::Warning, "PageStore",
                        "roadField R8 array creation FAILED; field plane "
                        "disabled");
            config_.roadFieldRequest = nullptr;
        }
    }
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
    it->second.fieldToken.cancel();  // 场生产同理(迟到 r8 经账本校验丢弃)
    if (it->second.compose) {
        // 在途合成任务省功早退;迟到快照由 drainReadyUploads 账本校验丢弃。
        it->second.compose->cancelled.store(true, std::memory_order_release);
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
    const size_t maxSlot = fieldArrayTexture_
                               ? static_cast<size_t>(kGltfRoadFieldTextureSlot)
                               : static_cast<size_t>(
                                     kGltfPageStoreIndirTextureSlot);
    if (cmd.textures.size() <= maxSlot) {
        cmd.textures.resize(maxSlot + 1u, nullptr);
    }
    cmd.textures[kGltfPageStoreArrayTextureSlot] = arrayTexture_.get();
    // 间接纹理 array 绑 slot21,片元经层号 fetch 定位 layer + 读 A 通道作 miss
    // 回退 factor。
    cmd.textures[kGltfPageStoreIndirTextureSlot] = indirArrayTexture_.get();
    // enabled=1 + cell 网格(单位=源瓦片)+ texcoord 集 → 片元采页存储。
    // layer 由间接纹理 RG 承载,resident/miss 由其 A 承载。
    //
    // 这里原样搬 determination 算好的 placement,**不重新推导**:cell 网格是不是
    // 几何等分,只有 determination 知道(它才见得到 overlay 的生效投影与 details
    // 矩形)。命令构建侧自己算 = 又一个「几何格 == 源格」的独立假设,而这正是
    // GCJ 底图整条特性静默失效的根。
    // w 打包 texCoordSet + 祖先寻址相位。相位 = x0/y0 对 2^kMaxDetDepthLevels
    // 取模:per-cell 渐变 LOD 的祖先子区原点 floor(cell/2^d)*2^d 必须在**全局源
    // 瓦片下标**上算,而片元只有局部 cell 下标。改造前 x0=key.x*gridN 恒是 2^d
    // 的倍数(代码里那句「右移无进位污染」),局部=全局所以没人发现;搬进源网格后
    // x0 成了任意整数,d>0 的 cell 会采到祖先页里错的子区 —— 真机上是块状明暗
    // 棋盘格。标准 overlay 下 span | gridN | x0 → 相位对 span 取模恒 0,逐字节不变。
    const int phaseX = ind.placement.x0 & (kMaxDetSpan - 1);
    const int phaseY = ind.placement.y0 & (kMaxDetSpan - 1);
    cmd.gltfUniforms.pageStoreParams = {
        1.0f,
        static_cast<float>(ind.placement.cellsX),
        static_cast<float>(ind.placement.cellsY),
        static_cast<float>(ind.texCoordSet) +
            8.0f * static_cast<float>(phaseX) +
            512.0f * static_cast<float>(phaseY)};
    cmd.gltfUniforms.pageStoreUv = {
        static_cast<float>(ind.placement.originU),
        static_cast<float>(ind.placement.originV),
        static_cast<float>(ind.placement.spanU),
        static_cast<float>(ind.placement.spanV)};
    cmd.gltfUniforms.terrainLayers[1] = static_cast<float>(ind.layer);
    cmd.terrainPageStoreFullyResident = ind.fullyResident;
    // [瓦界对齐] 几何 UV→源格仿射:batcher(实例记录)与逐瓦位移地形 FS
    // (u_pageGeomA/B)共用同一套 → 合批态翻转零视觉差。真实网格 glTF FS 不消费。
    std::copy(ind.geomAffine, ind.geomAffine + 6,
              cmd.terrainPageGeomAffine.begin());
    cmd.gltfUniforms.pageGeomA = {ind.geomAffine[0], ind.geomAffine[1],
                                  ind.geomAffine[2], ind.geomAffine[3]};
    cmd.gltfUniforms.pageGeomB = {ind.geomAffine[4], ind.geomAffine[5], 0.0f,
                                  0.0f};

    // 刀2 场"第二平面":与影像页同一次间接查找(同 layer/sampleUv/祖先
    // scale-bias),FS 只多一次采样 + smoothstep。占位清场保证层复用时旧
    // 租户路网不外泄(见 kickPageFetches),这里无需 per-cell 门控 ——
    // indir 的 resident(A 通道)已同时门控两个平面。
    if (fieldArrayTexture_) {
        cmd.textures[kGltfRoadFieldTextureSlot] = fieldArrayTexture_.get();
        cmd.gltfUniforms.roadFieldParams[0] = 1.0f;
        cmd.gltfUniforms.roadFieldParams[1] = 4.0f;  // kLineFieldFeatherTexels
        cmd.gltfUniforms.roadFieldColor = config_.roadFieldColor;
    }
}

void TerrainPageStore::tick() {
    if (!arrayTexture_) {
        return;
    }
    ++frameId_;  // 推进帧号(下帧 determination 的 LRU touch/淘汰基准)
    const double tickStartMs = perf::nowMs();
    drainInbox();        // 阶段 A:原始影像 → worker 合成任务(或就地跑)
    drainReadyUploads();  // 阶段 B:worker 快照 → 预算内上传 + 叠画
    {
    }
    // compose 列自异步化起计的是 **worker 侧**耗时(判归属用);maxTick 只含
    // 渲染线程(dispatch+upload)—— 下放成功的判据就是它塌下来。
    winComposeMs_ +=
        static_cast<double>(readyInbox_->composeMicros.exchange(
            0, std::memory_order_relaxed)) /
        1000.0;
    winMaxTickMs_ = std::max(winMaxTickMs_, perf::nowMs() - tickStartMs);
    // 220ms 归属拆分:每 60 tick 报窗口累计与单帧峰值。**看的是归属比例**:
    // compose 占大头 → 下 worker;upload 占大头 → 降 maxUploadsPerFrame/换
    // 部分更新。纯运动期 items 应趋 0,
    // 持续 >0 = 在做无谓重合成(maplibre 式源集合指纹是下一步)。
    if (frameId_ % 60u == 0u) {
        platformLog(LogLevel::Info, "PageStore",
                    "tick60 compose=%.1fms upload=%.1fms "
                    "maxTick=%.1fms items=%d fields=%d",
                    winComposeMs_, winUploadMs_,
                    winMaxTickMs_, winInboxItems_, winFieldUploads_);
        winComposeMs_ = 0.0;
        winUploadMs_ = 0.0;
        winMaxTickMs_ = 0.0;
        winInboxItems_ = 0;
        winFieldUploads_ = 0;
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

    // ==== 刀2 场平面:与影像 fetch 并行 kick ====
    if (config_.roadFieldRequest && fieldInbox_) {
        const int side = config_.pageSizeTexels;
        if (layer >= 0 && layer < static_cast<int>(fieldLayerKey_.size()) &&
            fieldLayerKey_[layer] == pageKey) {
            // **同 key 页 churn 重建**(淘汰后同 key 复用同层):层里仍装本页
            // 真场,内容正确 → 不重烘、不清、fieldReady 立即 true(间接纹理
            // A=255 继续采旧真场)→ 运动期不闪。这正是占位清场治不了的病:
            // 占位会把这份正确真场清成 0,新真场到达前无线。
            entry.fieldPending = false;
        } else {
            // 新层 / 不同 key 抢层:场 pending。pending 期该 layer 的 fieldReady
            // 为 false → 间接纹理 A=128 → 片元采影像不采场(不显示旧租户残留/
            // 新层垃圾)。真场到达 drainReadyUploads 设 fieldLayerKey_ 并放行。
            entry.fieldToken = CancellationToken{};
            entry.fieldPending = true;
            std::shared_ptr<RoadFieldInbox> fieldInbox = fieldInbox_;
            config_.roadFieldRequest(
                pageTileKey, entry.fieldToken,
                [fieldInbox, pageKey, layer, side](std::vector<uint8_t> r8) {
                    if (r8.size() !=
                        static_cast<size_t>(side) * static_cast<size_t>(side)) {
                        return;  // 尺寸不符:丢弃(防生产者配置漂移写坏层)
                    }
                    // 回调只捕 inbox shared_ptr —— 页存储先亡不悬垂,迟到结果
                    // 经账本校验丢弃。
                    std::lock_guard<std::mutex> lock(fieldInbox->mutex);
                    fieldInbox->items.push_back(
                        RoadFieldInbox::Item{pageKey, layer, std::move(r8)});
                });
        }
    }
}

void TerrainPageStore::drainInbox() {
    // 阶段 A(派发):原始解码影像 → worker 合成任务。渲染线程只做账本校验
    // (页在/层配)与派发;重采样+alphaOver 全在 worker(真机测得 compose 是
    // tick 支配成本且单帧尖刺 33-37ms)。pool 为空(host 测试)就地跑 ——
    // 行为与下放前逐字节一致,只是同帧完成。
    std::vector<PendingInbox::Item> arrived;
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        arrived.swap(inbox_->pages);
    }
    const int side = config_.pageSizeTexels;
    for (auto& item : arrived) {
        // C-1b:**不再要求源尺寸等于页边长**。resamplePageSource 按 image 自身
        // 尺寸重采样,任意 tileSize 的 provider 都能进页。旧护栏「非 256² 跳过」
        // 是静默丢弃 —— 真机踩过:矢量源 tileSize=512,图非空(故不打 NULL 日志)
        // 却恒被丢,表现为该源在页内完全不存在,且没有任何一条错误日志。
        DecodedImage* image = item.image.get();
        if (!image || image->width <= 0 || image->height <= 0 ||
            image->pixels.empty()) {
            continue;
        }
        // 校验页仍驻留且 layer 匹配(淘汰/换租后 layer 变或页消失 → 丢弃)。
        // 上传前 drainReadyUploads 会再验一次 —— 这里挡的是白干,那里挡的是写错层。
        const auto it = pages_.find(item.key);
        if (it == pages_.end() || it->second.layer != item.layer ||
            !it->second.compose) {
            continue;
        }
        ++winInboxItems_;
        // 任务自持有全部状态(shared_ptr):页存储先亡也不悬垂。ThreadPool 的
        // std::function 需可拷贝 → image 转 shared_ptr。
        auto compose = it->second.compose;
        auto readyInbox = readyInbox_;
        std::shared_ptr<DecodedImage> sharedImage = std::move(item.image);
        const uint64_t pageKey = item.key;
        const int layer = item.layer;
        const int source = item.source;
        const int depth = item.ancestorDepth;
        const int subX = item.subX;
        const int subY = item.subY;
        auto task = [compose, readyInbox, sharedImage, pageKey, layer, source,
                     depth, subX, subY, side]() {
            if (compose->cancelled.load(std::memory_order_acquire)) {
                return;  // 页已淘汰:省功(安全线在 drainReadyUploads 校验)
            }
            const double composeStartMs = perf::nowMs();
            std::vector<uint8_t> rgba;
            resamplePageSource(*sharedImage, depth, subX, subY, side, rgba);
            ReadyUploadInbox::Item out;
            bool accepted = false;
            {
                std::lock_guard<std::mutex> lock(compose->mutex);
                // C-1:按源序合成;乱序早到进 stash、重复到达幂等丢弃 ——
                // 多个 worker 线程的执行次序因此无关紧要(mutex 只保原子性,
                // 次序正确性由 assembler 自身的 stash 语义保证)。
                accepted = compose->assembler.accept(source, rgba.data());
                if (accepted) {
                    out.texels = compose->assembler.texels();  // 快照
                    out.composedSources = compose->assembler.compositedCount();
                    if (compose->assembler.complete()) {
                        compose->assembler.releaseBuffers();  // 稳态零额外内存
                    }
                }
            }
            readyInbox->composeMicros.fetch_add(
                static_cast<uint64_t>(
                    (perf::nowMs() - composeStartMs) * 1000.0),
                std::memory_order_relaxed);
            if (!accepted) {
                return;
            }
            out.key = pageKey;
            out.layer = layer;
            std::lock_guard<std::mutex> lock(readyInbox->mutex);
            readyInbox->items.push_back(std::move(out));
        };
        if (config_.composeWorkers) {
            config_.composeWorkers->enqueue(std::move(task));
        } else {
            task();
        }
    }
}

void TerrainPageStore::drainReadyUploads() {
    // 阶段 B(上传+叠画,渲染线程):取 worker 快照,按预算 updateTextureRegion。
    // uploadedSources 在此推进 —— determination 的 resident 判定跟它走,
    // 保证间接纹理永远只指向已写入的层。
    const int side = config_.pageSizeTexels;
    int uploaded = 0;

    // ==== 刀2 场平面:与影像共享 maxUploadsPerFrame 预算(场 64KB/页,记
    // 1 个上传)。上传真场后设 fieldLayerKey_[layer]=key + 清 fieldPending →
    // determination 下帧判该层场 ready(间接纹理 A=255)放行采样。====
    if (fieldInbox_ && fieldArrayTexture_) {
        std::vector<RoadFieldInbox::Item> fieldReady;
        {
            std::lock_guard<std::mutex> lock(fieldInbox_->mutex);
            fieldReady.swap(fieldInbox_->items);
        }
        size_t fieldRequeueFrom = fieldReady.size();
        for (size_t idx = 0; idx < fieldReady.size(); ++idx) {
            if (uploaded >= config_.maxUploadsPerFrame) {
                fieldRequeueFrom = idx;
                break;
            }
            auto& item = fieldReady[idx];
            const auto it = pages_.find(item.key);
            if (it == pages_.end() || it->second.layer != item.layer) {
                continue;  // 淘汰/换租后的迟到场:丢弃
            }
            const double uploadStartMs = perf::nowMs();
            device_->updateTextureRegion(
                fieldArrayTexture_.get(), 0, 0, side, side, item.r8.data(),
                static_cast<size_t>(side), item.layer);
            winUploadMs_ += perf::nowMs() - uploadStartMs;
            // 记住该层现装的是这个 key 的真场 → 同 key 淘汰重建可跳烘不闪;
            // 不同 key 抢层前该值仍是旧 key(pending 门控靠它挡住旧残留)。
            if (item.layer >= 0 &&
                item.layer < static_cast<int>(fieldLayerKey_.size())) {
                fieldLayerKey_[item.layer] = item.key;
            }
            it->second.fieldPending = false;
            ++winFieldUploads_;
            it->second.lastProgressFrame = frameId_;
            ++uploaded;
        }
        if (fieldRequeueFrom < fieldReady.size()) {
            std::lock_guard<std::mutex> lock(fieldInbox_->mutex);
            fieldInbox_->items.insert(
                fieldInbox_->items.begin(),
                std::make_move_iterator(fieldReady.begin() + fieldRequeueFrom),
                std::make_move_iterator(fieldReady.end()));
        }
    }

    std::vector<ReadyUploadInbox::Item> ready;
    {
        std::lock_guard<std::mutex> lock(readyInbox_->mutex);
        ready.swap(readyInbox_->items);
    }
    if (ready.empty()) {
        return;
    }
    size_t requeueFrom = 0;
    for (size_t idx = 0; idx < ready.size(); ++idx) {
        if (uploaded >= config_.maxUploadsPerFrame) {
            requeueFrom = idx;  // 超本帧预算:剩余下帧再传(涓流,勿冻结拖动)
            break;
        }
        auto& item = ready[idx];
        requeueFrom = idx + 1;
        // 安全线:页仍驻留且 layer 匹配才写(淘汰/换租后的迟到快照丢弃)。
        const auto it = pages_.find(item.key);
        if (it == pages_.end()) {
            continue;
        }
        PageEntry& pe = it->second;
        if (pe.layer != item.layer) {
            continue;
        }
        // 同页多快照乱序到达:进度只前进不后退(旧快照晚到不覆盖新画面)。
        if (item.composedSources <= pe.uploadedSources) {
            continue;
        }
        const double uploadStartMs = perf::nowMs();
        device_->updateTextureRegion(arrayTexture_.get(), 0, 0, side, side,
                                     item.texels.data(),
                                     static_cast<size_t>(side) * 4u,
                                     item.layer);
        winUploadMs_ += perf::nowMs() - uploadStartMs;
        pe.uploadedSources = item.composedSources;
        pe.lastProgressFrame = frameId_;  // 上传推进 = 进度(见 kStalledPageFrames)
        ++uploadedLayerTotal_;
        ++uploaded;
    }
    // 未处理完的项(超预算)放回下帧继续。
    if (requeueFrom < ready.size()) {
        std::lock_guard<std::mutex> lock(readyInbox_->mutex);
        for (size_t idx = requeueFrom; idx < ready.size(); ++idx) {
            readyInbox_->items.push_back(std::move(ready[idx]));
        }
    }
}

}  // namespace earth_engine
