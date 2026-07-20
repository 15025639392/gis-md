#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include "../core/math/Rectangle.h"
#include "TileKey.h"

namespace earth_engine {

class RenderDevice;
class Buffer;

// 共享模板固定栅格单元数（n=65=2^6+1，GE 嵌套栅格约定；与 grid64 一致）。
// 模板独立于瓦片原始网格密度——所有地形瓦片用同一密度共享模板，UV 均匀。
inline constexpr int kTerrainDisplacementGridSize = 64;

// 北极星 Phase 2c（地形 GPU 位移）：共享位移模板的 GPU 缓冲池。
//
// 同 {schemeId, z, row(y), gridSize} 的瓦片共享一份模板 VBO/IBO——模板顶点在
// 瓦片中心 ENU 局部帧、逐列不变（见 content/TerrainDisplacementTemplate），故
// 整行只上传一次，令地形 VBO 字节从「随可见瓦片数线性」压到「每 {LOD,row} 一
// 份」（北极星 §5 有界闸门）。每瓦片的经纬度落位由 per-tile 刚体帧承担（在
// GltfDrawCommandBuilder 算 enuToEcef(center) 写进 RenderCommand），不进本池。
//
// 池由 Engine 惰性持有（terrainGpuDisplacement flag 开启时）；draw 层经
// Renderer::terrainDisplacementPool() 非空即启用（仿 TerrainPageStore 门控）。
// 模板顶点打包成与现有路径逐字节一致的 32B TerrainGpuVertex（heightDelta=0，
// Stage A 零起伏；Stage B 由高度纹理在 shader 里位移）。
class TerrainDisplacementTemplatePool {
public:
    struct TemplateBuffers {
        Buffer* vertexBuffer = nullptr;
        Buffer* indexBuffer = nullptr;
        int indexCount = 0;
        int vertexCount = 0;
    };

    void initialize(RenderDevice* device) { device_ = device; }
    bool ready() const { return device_ != nullptr; }

    // 取（必要时建+上传）key 瓦片所在 {LOD,row} 的共享模板。用 bounds 生成
    // （列无关，任一代表列即可），按 {schemeId,z,y,gridSize} 缓存复用。
    // device 未初始化或 gridSize<1 返回 nullptr。
    const TemplateBuffers* acquire(const TileKey& key, const Rectangle& bounds,
                                   int gridSize);

    size_t residentTemplateCount() const { return cache_.size(); }
    // 已上传模板 VBO 总字节（§5 有界性观测：应随可见 {LOD,row} 数封顶）。
    size_t totalVertexBytes() const { return totalVertexBytes_; }

private:
    struct Entry {
        std::unique_ptr<Buffer> vertexBuffer;
        std::unique_ptr<Buffer> indexBuffer;
        TemplateBuffers view;
    };
    static uint64_t cacheKey(const TileKey& key, int gridSize);

    RenderDevice* device_ = nullptr;
    std::unordered_map<uint64_t, Entry> cache_;
    size_t totalVertexBytes_ = 0;
};

}  // namespace earth_engine
