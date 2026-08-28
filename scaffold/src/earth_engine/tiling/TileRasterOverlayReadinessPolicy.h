#pragma once

#include <cstddef>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class RasterOverlayFrameContext;
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

// NoReadyTexture 的细分画像。残余破洞全部落在这一桶里(nomap=0 已排除时序),
// 但"绑不到纹理"至少有三种完全不同的病:
//   ancestorDepth == 0                      → 这片就是根,压根没有祖先可借
//   ancestorsWithMapping == 0               → 祖先在,但没给它们建过 mapping
//   ancestorsWithMapping>0 && withTexture==0 → mapping 在、纹理没了(淘汰/没上传)
// 三者修法互斥,不插桩拿证据就只能猜(上一轮"常驻粗影像"就是猜错的产物)。
struct BaseImageryNoTextureProbe {
    bool valid = false;
    int zoom = -1;
    // RasterOverlayTile::LoadState 的原始整数值;-1 = 该指针为空。
    int loadingState = -1;
    int readyState = -1;
    bool readyHasTexture = false;
    // 从 parent 起一路走到根,统计这条链上本 overlay 的 mapping 情况。
    int ancestorDepth = 0;
    int ancestorsWithMapping = 0;
    int ancestorsWithTexture = 0;
    // 第二级:第一轮实测发现 load=Loaded/Done 却 ready=空 —— 影像已经在手却没被
    // 提升为 ready。要分清是「update 根本没跑」还是「跑了却被打回」:
    //   mappingState        DirectRasterMapping::State(0 未挂 /2 已挂)
    //   authoritativeUpdates 该瓦片 overlay 状态的权威更新次数(逐帧不涨 = 没跑)
    //   tileLoadState / tileContentKind  几何瓦片自身的状态(Done+Render 才留 mapping)
    int mappingState = -1;
    unsigned long long authoritativeUpdates = 0;
    int tileLoadState = -99;
    int tileContentKind = -1;
};

class TileRasterOverlayReadinessPolicy {
public:
    static bool doneTileCannotHoldRasterOverlays(const TilesetTile& tile);

    /// 对第一个被 NoReadyTexture 拦住的 base imagery overlay 取画像。
    /// tile 不是该成因时返回 valid=false。
    static BaseImageryNoTextureProbe probeNoReadyTexture(
        const TilesetTile& tile,
        const RasterOverlayFrameContext& frame);

    /// requiredBaseImageryDrawableReady 的成因版:返回第一个拦住这片瓦片的
    /// 原因,None 表示可画。前者按后者实现,二者不会漂。
    static BaseImageryBlockReason baseImageryBlockReason(
        const TilesetTile& tile,
        const RasterOverlayFrameContext& frame);

    static bool requiredOverlaysReady(
        const TilesetTile& tile,
        const RasterOverlayFrameContext& frame);

    static bool requiredBaseImageryDrawableReady(
        const TilesetTile& tile,
        const RasterOverlayFrameContext& frame);

    static bool terrainSurfaceImageryDrawableReady(
        const TilesetTile& tile,
        const RasterOverlayFrameContext& frame);

    static std::vector<size_t> processingOrder(
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays);
};

} // namespace earth_engine
