#pragma once

#include "../core/math/Rectangle.h"
#include "TileKey.h"

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {

struct TilesetTile;
class TileScheme;
class TilesetTileRegistry;

/// CPU 侧统一高程采样服务(每 Tileset 一个,仅渲染线程)。
///
/// 定位:在既有 retained DecodedHeightmap(渲染真值,GPU 位移的同一数据源)
/// 之上建一层"带索引、带质量标签"的查询门面——零数据拷贝,不自建金字塔,
/// 不做 IO;miss 即 nullopt,由调用方显式决定兜底语义(value_or)。
///
/// 索引:按 zoom 分层的 cell 哈希,key 由 TileScheme::positionToTile 计算。
/// 惰性重建:heightmapGeneration(全局强代次,见 TileRenderContentState)变化
/// 时才从 registry 全量重建 O(n)——稳态零重建;对比旧路径"每个点查询 O(n)
/// 扫全表",加载期一帧最多一次重建也是严格占优。
///
/// 生命周期安全:索引存 TilesetTile 裸指针。前提是两条已验证的事实——
///   1. registry 从不 erase 单个瓦片(瓦片骨架与 Tileset 同生共死,unload
///      只清 content);
///   2. 本服务是 Tileset 成员,tileset 亡则服务同亡。
/// 因此裸指针不可能悬垂。heightmap 消亡(content unload)≠ 瓦片销毁,由
/// 查询时二次验真兜住(见下)。
///
/// 正确性不赌索引新鲜度:命中 cell 后仍在查询时验 isTerrainRenderContent
/// + heightmap valid + bounds.contains——索引只是"提示",过期条目零危害。
///
/// 不变量与溢出列表:cell 索引假设"terrain 瓦片 bounds == scheme 矩形"
/// (上采样 bounds 毒化已根修的生产不变量)。重建时逐瓦验证,不满足者进
/// irregular 溢出列表按旧语义线性扫——正确性从不依赖不变量,性能才依赖。
/// irregularCount() 暴露给诊断,生产稳态应恒 0。
///
/// 采样语义与旧 LoadedTerrainHeightSampler 逐点等价(对拍守卫见
/// test_terrain_height_service.cpp):最深档优先、同深取更高、无覆盖
/// nullopt;RenderGridConsistent 只对齐单个 registry 瓦片的渲染网格档位。
/// 它不是本帧可见面查询：不表达 TileRenderEntry 选择、祖先裁剪回退、
/// relief fade 或 geomorph。矢量贴地必须使用 RenderedTerrainSurfaceSampler。
class TerrainHeightService {
public:
    enum class Interp {
        /// 全分辨率双线性(数据侧真值;fill 代理抬升等既有语义)。
        FullResBilinear,
        /// 单瓦片渲染网格一致分段线性；相机/通用 registry 查询使用。
        /// 本帧可见面消费者不得使用此枚举冒充 TilePlan 合同。
        RenderGridConsistent,
    };

    /// 采样结果自带质量标签:zoom = 答案来源瓦片的档位。调用方据此区分
    /// "z15 真值"与"z5 祖先凑的"(加载期回退是概率性的,不是结构性保证)。
    struct Sample {
        float height = 0.0f;
        int zoom = -1;
    };

    /// registry/scheme 由所属 Tileset 持有,生命周期覆盖本服务。
    TerrainHeightService(const TilesetTileRegistry& registry,
                         const TileScheme& scheme);

    /// (lng, lat) 弧度处的高度,取最深的、带可用 heightmap 的覆盖瓦片;
    /// 无任何覆盖返回 nullopt(与真实海平面 0 明确区分)。
    std::optional<Sample> sample(double longitudeRadians,
                                 double latitudeRadians,
                                 Interp interp) const;

    /// 覆盖 area 的地形实测高度区间 (min, max)。
    ///
    /// 用途:一块**没有自己内容**的计划瓦片,包围体永远停在占位常量
    /// (-1000/9000) —— 收紧只发生在瓦片载入自己的内容之后。但它上屏的
    /// 几何并非凭空来的,数据就在别的瓦片里;这里按**矩形**去问,而不是
    /// 按 TileKey 上溯:计划瓦片的 key 未必与带高度图的瓦片同属一套网格
    /// (真机实测:计划 z12/3259/1697 的 z8 祖先应是 203/106,而索引里
    /// 是 202/107 —— 按 key 走的祖先链根本对不上)。矩形是两套网格之间
    /// 唯一共通的坐标。
    ///
    /// 取档规则:深→浅,只接受**整块 area 都被该档索引覆盖**的那一档。
    /// 部分覆盖就下一档 —— 缺的那部分若被漏掉,区间会比真实地面窄,而
    /// 贴地体窄了穿不透地形是**整片消失**,不是变淡。
    /// 无任何档能完整覆盖返回 nullopt。
    std::optional<std::pair<double, double>> heightRangeForArea(
        const Rectangle& area) const;

    /// 地形高度世界的全局强代次(TileRenderContentState::heightmapGeneration
    /// 的轻头文件直通)。相机探针失效、矢量重钳节流与本服务的索引重建共用
    /// 这一个信号源;替代 contentBytesUsed 弱代理。
    static std::uint64_t heightmapGeneration();

    /// E6 查询统计:miss(完全无覆盖)与命中档位分布。miss 率进 Policy
    /// (HeightSampleCoverage);档位分布只出 Info 诊断行——引擎内没有可
    /// 辩护的"目标档"概念(渲染档因迟滞/上采样与数据档合法地不一致),
    /// 原始分布留给分析期对照场景解读,不发明没依据的比率。
    struct SampleStats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        /// 命中答案的 zoom 直方图;超出末桶并入末桶。
        std::array<std::uint32_t, 24> zoomHits{};
        std::uint64_t total() const { return hits + misses; }
    };

    /// 取走并清零自上次调用以来的查询统计(与诊断报表同周期调用)。
    SampleStats takeSampleStats() const;

    /// 诊断:bounds != scheme 矩形而进溢出列表的瓦片数(生产稳态应恒 0)。
    std::size_t irregularCount() const;

    /// 诊断:索引里的瓦片数(带 heightmap 的地形瓦片)。**索引为空**与
    /// 「索引有货但查询点没覆盖」在 sample()/heightRangeForTile() 的返回值上
    /// 读数完全相同(都是 nullopt),而两者的病因与修法毫无关系。
    std::size_t indexedCount() const;

    /// 诊断:自构造以来的索引重建次数(稳态帧不应增长)。
    std::uint64_t rebuildCount() const { return rebuildCount_; }

private:
    struct ZoomLevel {
        // packed (x<<32|y) → tile。仅含"bounds==scheme矩形"的 terrain 瓦片。
        std::unordered_map<std::uint64_t, const TilesetTile*> cells;
    };

    void refreshIfStale() const;
    void rebuild() const;

    const TilesetTileRegistry* registry_ = nullptr;
    const TileScheme* scheme_ = nullptr;

    // 惰性重建的索引状态(逻辑 const 查询下可变;仅渲染线程访问,无锁)。
    mutable std::vector<ZoomLevel> levels_;          // 下标 = zoom
    mutable std::vector<int> populatedZoomsDesc_;    // 非空档位,深→浅
    mutable std::unordered_map<int, std::vector<const TilesetTile*>>
        irregularByZoom_;                            // 不变量溢出(应恒空)
    mutable std::uint64_t builtGeneration_ = 0;      // 0 = 从未建过
    mutable std::uint64_t rebuildCount_ = 0;
    mutable SampleStats sampleStats_;                // E6 窗口统计(渲染线程)
};

} // namespace earth_engine
