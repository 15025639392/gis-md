#pragma once

#include "ProviderRequestDiagnostics.h"
#include "../tiling/TileAvailabilityState.h"
#include "../tiling/TileKey.h"
#include "../tiling/TileLoadStatus.h"
#include "../tiling/SurfaceTile.h"
#include "../platform/bridge/PlatformBridge.h"
#include "../threading/CancellationToken.h"

#include <optional>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <array>
#include <utility>

namespace earth_engine {

class TilesetContentProvider;

/// 解码后的高度图数据。
/// 高度为 WGS84 ellipsoid height（meter）。
/// 网格为 tileSize×tileSize 的 regular grid。
struct DecodedHeightmap {
    int tileSize = 0;                  // 每边采样数（如 256）

    /// tileSize × tileSize，行优先（北→南）的 **16bit 量化**高度。
    /// 码 0 保留给 no-data；码 1..65535 表示到 `quantBase` 的格点偏移。
    ///
    /// 为什么量化：514² 的 float32 是 1.06MB/瓦片，近景 ~90 个驻留瓦片就是
    /// 91MB CPU 常驻，与 192MB 内容缓存预算同量级（见 CpuAcct 走账）。
    ///
    /// ⚠️ **为什么是全局固定格点而不是逐瓦片 min/range**：逐瓦片量化会让同一个
    /// 物理高度在相邻两瓦片里落到不同的量化区间，解出来的 float 不再逐位相等 ——
    /// 而 borderInset=0.5 半像素内缩买到的正是「相邻瓦片边界采样值完全一致」
    /// 这条无缝不变量（真机对拍 maxdiff=0，见 borderInset 注释）。实测逐瓦片
    /// 量化把它打破到 0.012~0.028m。改成 `h = (quantBase + code) · kQuantStep`
    /// 后，同一物理高度在任何瓦片里都取到同一个整数格点 → 解码结果逐位相同，
    /// 不变量恢复。
    ///
    /// 步长 1/8 m 是二进制精确值（0.1m 不是），乘法无舍入分歧；量程
    /// 65534 × 0.125 = 8191.75m，覆盖任何真实瓦片的**瓦内**起伏。
    ///
    /// ⚠️ 码 0 = no-data 是**保留码**，不是"高度 0"。全零字节 = 整片 no-data，
    /// 不是整片海平面（同类坑见 TerrainDisplacementTemplatePool 的边 LUT）。
    std::vector<uint16_t> quantizedHeights;
    int32_t quantBase = 0;  // 格点原点（单位 = kQuantStep）
    float minHeight = 0.0f;
    float maxHeight = 0.0f;

    /// 量化步长（米）。二进制精确，跨瓦片共享 —— 见 quantizedHeights 注释。
    static constexpr float kQuantStep = 0.125f;
    static constexpr int32_t kMaxQuantCode = 65535;

    /// 采样返回的 no-data 高度。必须 >50000 以触发 isNoData 的快路径。
    static constexpr float kNoDataHeight = 1.0e6f;

    /// 按线性下标取高度（no-data 返回 kNoDataHeight）。
    float heightAt(size_t index) const {
        const uint16_t code = quantizedHeights[index];
        return code == 0
            ? kNoDataHeight
            : static_cast<float>(quantBase + static_cast<int32_t>(code)) *
                  kQuantStep;
    }

    /// **仅构造期**的 float 暂存缓冲。解码/测试往这里写原始高度，写完调
    /// `assignHeights()` 量化进 `quantizedHeights` 并把本缓冲释放掉 —— 因此
    /// 它不占稳态内存。未定型的高度图 `valid()` 恒 false（读不到高度会立刻
    /// 表现为该瓦片无地形，而不是静默读到 0 米海平面）。
    std::vector<float> stagedHeights;

    /// 把 `stagedHeights` 量化进 16bit 存储（**保留**暂存，可继续编辑后重新
    /// 定型 —— 增量构造用）。扫出**排除 no-data 的** min/max 定量化区间 ——
    /// 调用前必须先设好 `noDataValues`（判据要用）。这里是 minHeight/maxHeight
    /// 的**唯一**产地：此前两个解码分支各自抄了一遍扫描逻辑，改量化时极易只
    /// 改一处。
    void assignHeights();

    /// 释放构造期暂存。定型后不再编辑时调 —— **生产解码路径必调**，留着就等于
    /// float32 与 16bit 各存一份 = 量化白做。
    void releaseStagedHeights();

    /// 一次性版本：填暂存 → 定型 → 释放暂存。生产解码走这条。
    void assignHeights(const std::vector<float>& rawHeights);

    /// 无效高度哨兵值列表（OpenGlobus noDataValues）
    std::vector<float> noDataValues;

    /// 高度缩放因子（OpenGlobus _heightFactor），默认为 1.0
    float heightFactor = 1.0f;

    /// 边界内缩（像素）。sampleBilinear 把 u/v∈[0,1] 映射到像素
    /// [borderInset, tileSize-1-borderInset] 而非 [0, tileSize-1]。
    /// = 0：顶点栅格源（如自产 grid65，像素 0/N-1 就落在瓦片边界上）。
    /// = 0.5：cell-registered + 1px 重叠环源（如 Mapbox 514 Terrain-RGB，
    ///        512 个 cell 中心在像素 1..512、瓦片真实边界在半像素 0.5/512.5、
    ///        像素 0 与 513 是邻居重叠 backfill）。半像素内缩后相邻瓦片共享
    ///        边界采样值完全一致 → 无缝（已真机对拍验证 maxdiff=0）。
    float borderInset = 0.0f;

    bool valid() const { return tileSize > 0 && quantizedHeights.size() == static_cast<size_t>(tileSize * tileSize); }

    /// 检查高度是否为无效值（哨兵值匹配 或 值 > 50000）
    bool isNoData(float height) const;

    /// 双线性采样高度。
    /// @param u 列归一化坐标 [0,1]（西→东）
    /// @param v 行归一化坐标 [0,1]（北→南）
    float sampleBilinear(float u, float v) const;

    /// 同上但**不钳 u/v 到 [0,1]**，只钳像素下标。u/v 超出 [0,1] 但仍在
    /// ±overscanReach() 内时读到的是重叠环像素 = 真实邻瓦数据。
    /// 边界法线差分靠它跨过瓦片边界取到邻居侧的样本；超出余量后退化为钳边。
    float sampleBilinearUnclamped(float u, float v) const;

    /// 可越界采样的余量（归一化 u/v 单位）= borderInset / span。
    /// 0.5 内缩的 514 源 → 0.5/512（一圈重叠像素）；顶点栅格源（inset=0）→ 0，
    /// 即无邻居数据可取，边界差分只能退化单边。
    float overscanReach() const;
};

struct TerrainTileLoadResult {
    TileLoadStatus status = TileLoadStatus::Failed;
    std::unique_ptr<DecodedHeightmap> heightmap;

    static TerrainTileLoadResult successWithHeightmap(
        std::unique_ptr<DecodedHeightmap> hm) {
        TerrainTileLoadResult result;
        result.status = hm ? TileLoadStatus::Renderable
                           : TileLoadStatus::Failed;
        result.heightmap = std::move(hm);
        return result;
    }

    static TerrainTileLoadResult empty() {
        TerrainTileLoadResult result;
        result.status = TileLoadStatus::Empty;
        return result;
    }

    static TerrainTileLoadResult retryLater() {
        TerrainTileLoadResult result;
        result.status = TileLoadStatus::RetryLater;
        return result;
    }

    static TerrainTileLoadResult failed() {
        TerrainTileLoadResult result;
        result.status = TileLoadStatus::Failed;
        return result;
    }

    static TerrainTileLoadResult cancelled() {
        TerrainTileLoadResult result;
        result.status = TileLoadStatus::Cancelled;
        return result;
    }
};

/// 地形数据 Provider 抽象接口。
/// 负责 URL 构建、网络请求和高度图解碼。不持有 GPU 资源。
class TerrainProvider {
public:
    virtual ~TerrainProvider() = default;

    /// 唯一标识
    virtual std::string id() const = 0;

    /// Provider 类型
    virtual std::string type() const { return "terrain"; }

    /// 瓦片体系 ID
    virtual std::string schemeId() const = 0;

    /// 最小/最大 zoom
    virtual int minZoom() const = 0;
    virtual int maxZoom() const = 0;

    /// 瓦片尺寸（像素）
    virtual int tileSize() const = 0;

    /// Display credit / attribution for this terrain source.
    virtual std::string attribution() const { return ""; }

    /// Whether this provider can serve the tile. Providers with metadata
    /// availability should reject unsupported tiles before network request.
    virtual bool supportsTile(const TileKey& key) const {
        return availabilityState(key) == TileAvailabilityState::Available;
    }

    /// cesium-native LayerJsonTerrainLoader::tileIsAvailableInLayer equivalent.
    /// Available means the content can be requested now. Unknown means a
    /// metadata availability subtree has not been loaded yet.
    virtual TileAvailabilityState availabilityState(const TileKey& key) const {
        if (key.schemeId != schemeId() ||
            key.z < minZoom() ||
            key.z > maxZoom()) {
            return TileAvailabilityState::NotAvailable;
        }
        return TileAvailabilityState::Available;
    }

    /// 构建瓦片请求 URL
    virtual std::string buildUrl(const TileKey& key) const = 0;

    /// Estimated transport requests issued by requestTile for this logical
    /// terrain tile. Providers that fan out metadata/subtree fetches should
    /// include those requests so frame budgets reflect real network pressure.
    virtual int estimatedRequestFanout(const TileKey&) const { return 1; }

    using TerrainCallback = std::function<void(
        const TileKey&, TerrainTileLoadResult)>;

    /// Asynchronously request terrain tile content.
    virtual void requestTile(const TileKey& key,
                             CancellationToken token,
                             TerrainCallback callback,
                             HttpRequestPriority priority =
                                 HttpRequestPriority::Normal) = 0;

    virtual ProviderRequestDiagnostics requestDiagnostics() const {
        return {};
    }

    /// 同步解码高度图（用于测试/调试）
    virtual std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t* data, size_t len) = 0;
};

} // namespace earth_engine
