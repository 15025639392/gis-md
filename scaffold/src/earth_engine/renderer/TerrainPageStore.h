#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "../core/math/Rectangle.h"
#include "../threading/CancellationToken.h"
#include "../tiling/TileKey.h"

namespace earth_engine {

class RenderDevice;
class Texture;
class ActivatedRasterOverlay;
class RasterOverlayTileProvider;
struct RenderCommand;
struct TilesetTile;

/// 北极星合成方案「页存储」生产原型(门③ Step 3)。
///
/// 拥有一张 `texture2DArray`(每页一层,§13),把它挂到**一个** capped 粗地形
/// 瓦片的 terrain 绘制命令上,片元按页表 layer 单次采样 → 该瓦片显示高清影像
/// (①路径通)、其余瓦片逐字节走现状(②边界=现状)。
///
/// **Step 3a(已真机验收 PASS)= 合成图案填充**:各层灌可区分纯色,隔离验证
/// 「渲染路径 + array 绑定 + GLES array-bind 分支」整链真机点亮。
///
/// **Step 3b(本类当前形态)= 真实高清影像填充**:目标瓦片锁定后,异步拉它在
/// **更深 LOD**(z + kDepthLevels)的覆盖影像子瓦片(`ImageryProvider::requestTile`
/// → 解码 RGBA)填进各层 → capped 粗瓦片显示比其几何 LOD 更细的真实影像。
/// gridN×gridN = 2^kDepthLevels 覆盖,cell→layer 与 3a 同。合成图案作为影像
/// 到达前的占位(渐次被真实影像覆盖)。
///
/// 目标瓦片选择:屏幕空间误差最大(最近/最占屏)的真实地形瓦片。
class TerrainPageStore {
public:
    struct Config {
        int pageSizeTexels = 256;  // 每页(每层)边长(标准 XYZ 影像瓦片 256)
        int gridN = 4;             // 瓦片切 gridN×gridN 页 → gridN² 层;
                                   // gridN 必须 = 2^kDepthLevels(与影像深度对齐)
        int depthLevels = 2;       // 影像比目标瓦片深几级(gridN=1<<depthLevels)
    };

    TerrainPageStore() = default;
    ~TerrainPageStore();

    TerrainPageStore(const TerrainPageStore&) = delete;
    TerrainPageStore& operator=(const TerrainPageStore&) = delete;

    /// 创建 array 纹理并灌合成图案占位。失败返回 false(调用方短路)。
    bool initialize(RenderDevice* device, const Config& config);

    bool isReady() const { return arrayTexture_ != nullptr; }

    /// 每帧(渲染线程)驱动:目标锁定后 kick 一次异步影像 fetch;排空已到达的
    /// 解码影像 → updateTextureRegion 灌对应 layer。
    void tick();

    /// 在 applyPerFrameCommandState 里对每个 terrain 命令调用:若命令是真实地形
    /// 且是(或锁成)目标瓦片,绑定 array 纹理 + 写 pageStoreParams(enabled=1)。
    /// overlays 用于锁定时捕获影像 provider(拉真实高清影像的数据源)。
    void applyToTerrainCommand(
        RenderCommand& cmd, const TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& overlays);

    Texture* arrayTexture() const { return arrayTexture_.get(); }

private:
    struct PendingInbox;  // 定义在 .cpp:worker 回调安全投递解码影像

    void fillSyntheticPlaceholder();
    void kickImageryFetch();
    void drainInbox();

    RenderDevice* device_ = nullptr;
    Config config_{};
    std::unique_ptr<Texture> arrayTexture_;

    TileKey targetKey_{};
    double bestSse_ = 0.0;  // 已见最大屏幕空间误差(选最占屏瓦片为目标)
    bool targetLocked_ = false;

    // Step 3b 异步 fetch 状态。
    RasterOverlayTileProvider* provider_ = nullptr;  // 锁定时从 overlays 捕获
    Rectangle targetBounds_{0.0, 0.0, 0.0, 0.0};     // 目标瓦片地理范围(radian)
    int targetZ_ = 0;
    bool fetchKicked_ = false;
    int uploadedLayers_ = 0;                         // 已灌真实影像的层数
    CancellationToken fetchToken_;                   // dtor 取消在途 fetch
    std::shared_ptr<PendingInbox> inbox_;            // 跨线程投递箱(存活于回调)
};

}  // namespace earth_engine
