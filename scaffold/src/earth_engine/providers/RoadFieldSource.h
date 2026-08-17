#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "../data/MvtTileFetchCache.h"
#include "../data/VectorRasterStyle.h"
#include "../tiling/TileKey.h"
#include "../threading/CancellationToken.h"

namespace earth_engine {

class ThreadPool;

/// 路网 SDF 场的页级生产者(刀2):页 key → 覆盖矩形(可选 GCJ 平移)→
/// z14 祖先 MVT 瓦(共享 MvtTileFetchCache,与面 drape 同一批瓦零重复
/// fetch)→ LineFieldRasterizer CPU scatter 烘焙 → R8 buffer 回调。
///
/// 与 VectorDrapeImageryProvider 的关系:同一套"矩形→祖先瓦"数学
/// (MvtRectCoverage),但输出是 R8 距离场而非 RGBA 影像,不冒充
/// ImageryProvider、不进页存储的 alphaOver assembler —— 它是页存储
/// "第二平面"(fieldArrayTexture_)的旁路数据源,经
/// TerrainPageStore::Config::roadFieldRequest(std::function)注入,
/// 页存储对本类零依赖(host 测试注入 fake)。
///
/// 失败语义:fetch/decode 失败的瓦当作"无线"照烘(缺席瓦不贡献线段),
/// 全部失败产全 0 场(反向编码,0=远离一切线,失败安全值)—— 场平面永远
/// 收口,server 不在时视觉=无路网,与刀1 面的降级语义一致。
///
/// 拆除竞态:与刀1 同法 —— 聚合态 Assembly 自持(shared_ptr),线程池持
/// weak,锁不上就地烘;回调必到。
class RoadFieldSource {
public:
    struct Options {
        int dataMaxZoom = 14;
        int fieldSize = 256;  // 页原生边长
        /// 只消费 line 通道(lineColor.a>0 的层),见 LineFieldRasterizer。
        VectorRasterStyle style;
        bool gcj02SourceGrid = false;
    };

    /// r8 恒为 fieldSize² 字节(即使取消/失败也回调,调用方按账本丢弃)。
    using FieldCallback = std::function<void(std::vector<uint8_t> r8)>;

    RoadFieldSource(Options options,
                    std::shared_ptr<MvtTileFetchCache> tileCache,
                    std::shared_ptr<ThreadPool> rasterPool = nullptr);

    void requestField(const TileKey& pageKey, CancellationToken token,
                      FieldCallback callback);

    int fieldSize() const { return options_.fieldSize; }

    /// V26 一期:运行期换线分级样式(换肤)。线程安全(小锁);在途烘焙持旧
    /// 快照收口,新请求起用新样式。**只换生产侧**:页存储已烘的旧样式场页
    /// 不自动作废(且其跳烘门会静默复用旧层),调用方随后须
    /// Engine::invalidateRoadFieldPages() 触发场页重烘。线色/宽度 ramp 不在
    /// 此处——那是 FS uniform(Engine::setRoadFieldStyleUniforms,零重烘)。
    void setStyle(VectorRasterStyle style);

private:
    struct Assembly;
    static void completeIfReady(const std::shared_ptr<Assembly>& assembly);
    static void runAssembly(const std::shared_ptr<Assembly>& assembly);

    /// options_.style 的加锁快照(setStyle 可与 requestField 并发)。
    VectorRasterStyle styleSnapshot() const;

    Options options_;
    mutable std::mutex styleMutex_;  // 只护 options_.style,其余构造后只读
    std::shared_ptr<MvtTileFetchCache> tileCache_;
    std::shared_ptr<ThreadPool> rasterPool_;
};

} // namespace earth_engine
