#pragma once

#include "RasterOverlayTileProvider.h"
#include "../tiling/RasterOverlayProjection.h"
#include "../core/math/MathUtils.h"

#include <atomic>
#include <functional>
#include <memory>

namespace earth_engine {

// ============================================================
// 投影 + 图像合成纯函数(I-P4:从 RasterOverlayTileProvider.cpp 拆出)
// ============================================================
// 这些函数零 provider 私有状态依赖:输入全显式传参,输出全按值/移动返回。
// 拆出的动机=单文件 4865 行过长;保持行为逐字等价是硬约束(raster ctest 全绿)。

inline constexpr double kCompositingPi = 3.14159265358979323846264338327950288;
inline constexpr double kCompositingTwoPi = 2.0 * kCompositingPi;
inline constexpr double kCompositingMaxWebMercatorLat = 1.4844222297453324;
inline constexpr double kCompositingPixelTolerance = 0.01;

/// 源 scheme 的投影类型(EPSG:3857 → WebMercator,否则 Geographic)。
RasterOverlayProjection projectionForSourceScheme(const TileScheme& scheme);

/// scheme+georeference 解析最终投影;GCJ-02 请求与非 EPSG:3857 不匹配时
/// 明确降级并报警(见原实现注释:静默降级曾造成中国区 ~500m 偏移)。
RasterOverlayProjection projectionForScheme(
    const TileScheme& scheme,
    RasterOverlayGeoreference georeference);

/// 地理矩形 → provider 投影空间(合成入口统一投影,X/Y 对称)。
Rectangle projectGeographicToProvider(
    const Rectangle& rectangle,
    RasterOverlayProjection projection);

/// provider 投影空间 → 地理矩形。
Rectangle unprojectProviderToGeographic(
    const Rectangle& rectangle,
    RasterOverlayProjection projection);

/// 纬度 → WebMercator y(投影空间)。
double webMercatorY(double latRad);

bool isWebMercatorScheme(const TileScheme& scheme);
double projectedSouth(const TileScheme& scheme, const Rectangle& bounds);
double projectedNorth(const TileScheme& scheme, const Rectangle& bounds);
double projectedHeight(const TileScheme& scheme, const Rectangle& bounds);
double projectedVForLatitudeInternal(
    const TileScheme& scheme,
    const Rectangle& bounds,
    double lat);

// ============================================================
// 几何 ↔ 影像覆盖映射(I-P4 第四刀 4a:从 RasterOverlayTileProvider.cpp 收口)
// ============================================================
// getTile/resolveTile 与 loadMapped/pump/sourceTileList 共享的纯几何 helper。
// 零 provider 私有状态依赖,输入全显式传参。

/// 采样内缩 epsilon:跨度 1e-9 分之一,下限 1e-12(防退化线段变零宽)。
double inwardSampleEpsilon(double span);

/// 把退化成线/点的裁剪矩形沿覆盖区内扩出至少一个采样单位宽度/高度。
Rectangle expandClampedLineIntoCoverage(const Rectangle& bounds,
                                        const Rectangle& coverage);

/// 几何矩形 → 影像覆盖矩形:相交取交,不相交按 cesium 语义钳到最近覆盖边
/// (clampOutsideCoverage 时)。返回 nullopt 表示无可用映射。
std::optional<Rectangle> mapGeometryBoundsToImageryCoverage(
    const Rectangle& geometryBounds,
    const Rectangle& coverage,
    bool clampOutsideCoverage);

/// 是否允许把无交集的 overlay 钳到覆盖边采样(当前恒 true)。
bool shouldClampOutsideCoverage(const RasterOverlay* owner);

/// scheme 根层的覆盖矩形(所有 root 瓦片并集)。
Rectangle schemeCoverageRectangle(const TileScheme& scheme);

/// scheme 覆盖矩形 ∩ provider 覆盖矩形;无交时回落 scheme 矩形。
Rectangle effectiveCoverageRectangle(const TileScheme& scheme,
                                     const Rectangle& providerCoverage);

struct RasterSourceResult {
    TileKey key;
    Rectangle bounds;
    std::shared_ptr<const DecodedImage> image;
    std::optional<Rectangle> sourceSubset;
    RasterOverlayTile::MoreDetailAvailable moreDetailAvailable =
        RasterOverlayTile::MoreDetailAvailable::Unknown;
    std::vector<std::string> diagnostics;
    std::vector<std::string> credits;
    bool terminalFailure = false;
};

/// 合成四叉树源影像(几何瓦对应的多源 → 一张合成图)。
RasterOverlayTileProvider::CompositeImageResult combineQuadtreeSourceImages(
    const TileScheme& scheme,
    const Rectangle& targetBounds,
    std::vector<RasterSourceResult>&& sources);

/// mapped raster 源的合成入口(空祖先回落 → 空图)。
RasterOverlayTileProvider::CompositeImageResult composeMappedSourceImageSet(
    const TileScheme& scheme,
    const Rectangle& targetBounds,
    std::vector<RasterSourceResult>&& sources,
    bool emptyWhenOnlyAncestorFallback);

/// 单源 blit 到目标(同尺寸走 unsafeBlit,不同尺寸走双线性)。
void blitImage(DecodedImage& target,
               const Rectangle& targetRectangle,
               const DecodedImage& source,
               const Rectangle& sourceRectangle,
               const std::optional<Rectangle>& sourceSubset);

bool isDecodedImageUploadable(const DecodedImage& image);
bool isRasterCompositeSourceImage(const DecodedImage& image);
bool hasNonAncestorRasterSourceImage(
    const std::vector<RasterSourceResult>& sources);
int64_t decodedImageSizeBytes(const DecodedImage& image);
void appendCredits(std::vector<std::string>& target,
                   const std::vector<std::string>& credits);

// ---- I-P4 第二刀:depot 段共享的轻量工具(主文件 + RasterOverlaySourceDepot 共用) ----

/// 直接瓦片判定:几何矩形与源瓦矩形在容差内相等(避免 direct 路径误判)。
bool rectanglesEqualForDirectRasterTile(const Rectangle& a,
                                        const Rectangle& b);

/// 按 scheme id 重建快照(worker 回调需要的独立 scheme 实例)。
std::unique_ptr<TileScheme> createAsyncSchemeSnapshot(const TileScheme& scheme);

/// 节流名额唯一释放(exchange 决定唯一递减方)。
void releaseRasterThrottleSlotOnce(std::atomic<bool>& released,
                                   std::atomic<uint32_t>& activeLoads);

/// 父瓦片键(用于祖先回落链)。
TileKey parentTileKey(const TileKey& key);

/// 源瓦片缓存键(无 epoch 版 / epoch 前缀版)。
std::string sourceCacheKey(const TileKey& key);
std::string sourceCacheKey(uint64_t epoch, const TileKey& key);

/// 峰值字节记账(缓存/背压诊断)。
void trackPeakBytes(int64_t currentBytes, int64_t& peakBytes);

/// 在飞栅格负载递减(CAS 循环)。
void decrementActiveRasterTileLoads(std::atomic<uint32_t>& activeLoads);

/// 源加载成功/失败回调类型(depot 与 provider 共享)。
using MappedSourceLoadSuccess =
    std::function<void(std::unique_ptr<DecodedImage>,
                       std::shared_ptr<const DecodedImage>,
                       Rectangle,
                       RasterOverlayTile::MoreDetailAvailable,
                       std::vector<std::string>,
                       std::vector<std::string>)>;
using MappedSourceLoadFailure = std::function<void(std::vector<std::string>)>;

/// 源结果是否已解析(image 或终态失败)。
bool isResolvedRasterSourceResult(const RasterSourceResult& source);

}  // namespace earth_engine
