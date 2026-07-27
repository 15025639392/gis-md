#pragma once

#include <cstddef>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
struct TilesetTile;

// 底图影像"不可画"的成因。用于破洞诊断:选中瓦片因影像未就绪被 finalizer
// 丢弃时,区分「还没给它建 mapping」(时序问题)和「建了但连祖先纹理都没有」
// (真的缺常驻粗影像)—— 两者的修法完全不同。
enum class BaseImageryBlockReason {
    None = 0,
    // 该 overlay 在这片瓦片上根本没有 mapping(prefetch 还没轮到它)。
    NoMapping,
    // mapping 在,但没有任何带纹理的 ready 瓦片可绑(含仍在 loading、
    // 且没有可用祖先纹理的情形)。
    NoReadyTexture,
    // 绑定可用但 texcoord set 越界(几何侧问题,不是影像没到)。
    TexcoordInvalid,
};

class TileRasterOverlayReadinessPolicy {
public:
    static bool doneTileCannotHoldRasterOverlays(const TilesetTile& tile);

    /// requiredBaseImageryDrawableReady 的成因版:返回第一个拦住这片瓦片的
    /// 原因,None 表示可画。前者按后者实现,二者不会漂。
    static BaseImageryBlockReason baseImageryBlockReason(
        const TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays);

    static bool requiredOverlaysReady(
        const TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays);

    static bool requiredBaseImageryDrawableReady(
        const TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays);

    static bool terrainSurfaceImageryDrawableReady(
        const TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays);

    static std::vector<size_t> processingOrder(
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays);
};

} // namespace earth_engine
