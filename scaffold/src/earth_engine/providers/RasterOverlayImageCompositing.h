#pragma once

#include "RasterOverlayTileProvider.h"
#include "../tiling/RasterOverlayProjection.h"
#include "../core/math/MathUtils.h"

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

}  // namespace earth_engine
