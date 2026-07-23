#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "../core/math/Rectangle.h"
#include "../renderer/TerrainPageStore.h"  // TerrainPageLayerPool(层 LRU)
#include "TileKey.h"

namespace earth_engine {

class RenderDevice;
class Buffer;
class Texture;
struct DecodedHeightmap;

// 共享模板固定栅格单元数（n=65=2^6+1，GE 嵌套栅格约定；与 grid64 一致）。
// 模板独立于瓦片原始网格密度——所有地形瓦片用同一密度共享模板，UV 均匀。
inline constexpr int kTerrainDisplacementGridSize = 64;

// 北极星 Phase 2c:地形位移随 LOD 连续衰减到平（GE 式「从太空看是光滑椭球，
// 靠近才长出起伏」）。粗/远瓦片（低 z）真实 relief 被粗网格 faceting 成钻石
// 尖刺 + skirt 墙外露,本就不可分辨,压平;fade→0 完全跳过位移路径(平椭球),
// fade→1 全起伏。**单一事实源**:draw 侧(GltfDrawCommandBuilder 模板 swap)与
// prepare 侧(P5b 跳过废弃 per-tile VBO 的判据)必须用同一函数,两侧判据不一致
// 会导致「建了但不画」或「不建又要画」。
inline constexpr double kTerrainReliefFadeZLo = 6.0;  // z≤6 全压平
inline constexpr double kTerrainReliefFadeZHi = 9.0;  // z≥9 全起伏

inline float terrainReliefFade(int z) {
    const double t = std::clamp(
        (static_cast<double>(z) - kTerrainReliefFadeZLo) /
            (kTerrainReliefFadeZHi - kTerrainReliefFadeZLo),
        0.0, 1.0);
    return static_cast<float>(t * t * (3.0 - 2.0 * t));  // smoothstep
}

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

    // Stage B:per-tile 高度数据(gridN+1 方 RGBA8,RG 打包 16bit 归一化高度,
    // NEAREST)。shader 顶点级 texelFetch 取回、按 (minHeight,heightRange) 反量化
    // → pos = 面点 + 法线·h。
    //
    // 合批 Step 1:存储从「每瓦片一张 2D 纹理(无界增长)」改为**共享
    // texture2DArray**(每瓦片一层,固定 kHeightArrayLayers 层,层 LRU 复用
    // TerrainPageLayerPool)——实例化批内高度纹理经实例 layer id 寻址的前提;
    // 逐 draw 路径同步迁移(u_terrainLayers.x),不留双存储形态。
    //
    // 层淘汰一致性:每层带 epoch(重分配自增)。常驻 draw 命令记录其
    // (layer, epoch),draw 侧每帧用 heightLayerCurrent() 校验,失配 →
    // invalidate 命令缓存 → rebuild 重新 acquire(P5b 自愈模式)。可见瓦片
    // 每帧 touchHeightTexture 保活,当帧被 touch 的层绝不被淘汰(池语义),
    // 故只要容量 ≥ 峰值可见瓦片数(实测 ~185 << 256),淘汰只落在离屏瓦片。
    struct HeightTexture {
        Texture* texture = nullptr;  // 共享 array 纹理
        float minHeight = 0.0f;
        float heightRange = 1.0f;  // maxHeight − minHeight(下限保护 >0)
        int gridSize = 0;
        int layer = -1;            // 本瓦片高度数据所在层
        uint32_t epoch = 0;        // 该层分配代;层被重分配后旧 epoch 失效
    };
    // GLES3.0 GL_MAX_ARRAY_TEXTURE_LAYERS 规范下限 256(Adreno 实测 2048),
    // 按规范下限定容;峰值可见 ~185 层留有余量。65²×4B×256 ≈ 4.3MB。
    static constexpr int kHeightArrayLayers = 256;

    const HeightTexture* acquireHeightTexture(const TileKey& key,
                                              const DecodedHeightmap& heightmap,
                                              int gridSize,
                                              uint64_t frameId);

    // 可见瓦片每帧保活(recency + 当帧免淘汰)。key 未驻留则 no-op。
    void touchHeightTexture(const TileKey& key, uint64_t frameId);

    // 常驻命令缓存的 staleness 校验:该 (layer, epoch) 是否仍指向当初的瓦片。
    bool heightLayerCurrent(int layer, uint32_t epoch) const {
        return layer >= 0 &&
               static_cast<size_t>(layer) < heightLayerEpochs_.size() &&
               heightLayerEpochs_[static_cast<size_t>(layer)] == epoch;
    }

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
    static uint64_t heightCacheKey(const TileKey& key);  // 逐瓦片(含列)

    // 惰性建共享高度 array(首次 acquire;gridSize 定层边长)。失败返回 false。
    bool ensureHeightArray(int gridSize);

    RenderDevice* device_ = nullptr;
    std::unordered_map<uint64_t, Entry> cache_;
    size_t totalVertexBytes_ = 0;

    // ---- 高度纹理共享 array 存储(合批 Step 1)----
    std::unique_ptr<Texture> heightArray_;
    int heightArrayGridSize_ = 0;  // 建 array 时的 gridSize(层边长-1);不匹配拒绝
    TerrainPageLayerPool heightLayerPool_;  // 层 LRU(blockLayers=1)
    std::vector<uint32_t> heightLayerEpochs_;  // 每层分配代
    std::unordered_map<uint64_t, HeightTexture> heightIndex_;  // 瓦片键 → 驻留视图
};

}  // namespace earth_engine
