#pragma once

#include <cstdint>
#include <memory>

#include "../tiling/TileKey.h"

namespace earth_engine {

class RenderDevice;
class Texture;
struct RenderCommand;
struct TilesetTile;

/// 北极星合成方案「页存储」生产原型(门③ Step 3)。
///
/// 拥有一张 `texture2DArray`(每页一层,§13),把它挂到**一个** capped 粗地形
/// 瓦片的 terrain 绘制命令上,片元按页表 layer 单次采样 → 该瓦片显示高清影像
/// (①路径通)、其余瓦片逐字节走现状(②边界=现状)。
///
/// **Step 3a(本类当前形态)= 合成图案填充**:各层灌一个可区分的纯色,先隔离
/// 验证「渲染路径 + array 绑定 + GLES array-bind 分支」整链在真机点亮,把异步
/// 影像 fetch 的数据通路风险隔到 Step 3b。3b 再把填充换成
/// ImageryProvider::requestTile 拉的真实更深 raster 瓦片。
///
/// 目标瓦片选择:锁定绘制序里遇到的**第一个真实地形瓦片**(reproducible demo
/// 相机下稳定),之后恒定该 key。相机移开该瓦片离屏则页存储不显示(原型可接受)。
class TerrainPageStore {
public:
    struct Config {
        int pageSizeTexels = 256;  // 每页(每层)边长
        int gridN = 2;             // 瓦片切 gridN×gridN 页 → gridN² 层
    };

    TerrainPageStore() = default;
    ~TerrainPageStore();

    TerrainPageStore(const TerrainPageStore&) = delete;
    TerrainPageStore& operator=(const TerrainPageStore&) = delete;

    /// 创建 array 纹理并灌合成图案。失败返回 false(调用方短路,不影响渲染)。
    bool initialize(RenderDevice* device, const Config& config);

    bool isReady() const { return arrayTexture_ != nullptr; }

    /// 在 applyPerFrameCommandState 里对每个 terrain 命令调用:若命令是真实地形
    /// 且是(或锁成)目标瓦片,绑定 array 纹理 + 写 pageStoreParams(enabled=1)。
    /// 非目标/非真实地形一律不动(enabled 保持 0,走现状路径)。
    void applyToTerrainCommand(RenderCommand& cmd, const TilesetTile& tile);

    Texture* arrayTexture() const { return arrayTexture_.get(); }

private:
    RenderDevice* device_ = nullptr;
    Config config_{};
    std::unique_ptr<Texture> arrayTexture_;
    TileKey targetKey_{};
    double bestSse_ = 0.0;  // 已见最大屏幕空间误差(选最占屏瓦片为目标)
    bool targetLocked_ = false;
};

}  // namespace earth_engine
