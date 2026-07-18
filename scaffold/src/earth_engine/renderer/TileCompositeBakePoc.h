#pragma once

#include "RenderDevice.h"

#include <cstdint>
#include <memory>

namespace earth_engine {

// ============================================================
// 北极星 Phase 2b — B 方案(逐瓦片合成)PoC(骨架)
// ============================================================
//
// 目的 = **量 B(screen-sized 逐瓦片合成)的移动端每帧烘焙开销**,补齐 B vs C
// 决策缺的第三块数据(C 的回读税已量:近景/地平线均~1.2ms 中位/~9ms 尾)。
//
// B 的做法:每个 capped 地形瓦片,把覆盖它的深 z 影像烘进一张 screen-coverage
// 定尺的纹理(离屏 pass),再作该瓦片的影像绑定。**成本随 capped 瓦片数 N 线性**
// ——N 个离屏 bake pass(tiled GPU 上每个 = 一次 tile load/store 切换,移动端贵)
// + 每 pass 若干合成 draw。真机实测 capped 瓦片数:近景 3、地平线 122——B 在
// 地平线的 N=122 就是它相对 C(单 atlas)吃亏的地方,这个 PoC 就量它到底多贵。
//
// **本骨架量「N 个离屏 pass 切换 + 分配」的地板**(quadsPerTile=0):即便只量
// pass 切换,若地平线 N=122 的地板已 >> C 的~1.2ms,B 在地平线就定输(安全下界
// 结论)。合成 draw(采样覆盖影像)是下一步细化——与 C-PoC「feedback shader 未
// 接」对称的诚实缺口。bakeBytes 报 B 的逐瓦片纹理内存(N × 定尺²×4)。

struct TileCompositeBakePocConfig {
    // 每 capped 瓦片合成纹理边长(texels)。真实应按瓦片屏占定尺;骨架固定占位。
    int bakeTargetSize = 256;
    // 每帧烘焙瓦片数上限(防失控;地平线实测 ~122,留余量)。
    int maxBakeTilesPerFrame = 256;
    // 每瓦片的合成源 quad 数(每个全屏 quad 采样源影像着色整个 bake 目标 =
    // 一遍 fill)。1 = fill 地板(1 sample/px);调高模拟多覆盖瓦片叠加。0 = 只量
    // pass 切换。GLES 才生效(需 shader);Metal 未接 shader 时忽略。
    int quadsPerTile = 1;
};

struct TileCompositeBakeFrameStats {
    double bakeMs = 0.0;    // N 个离屏 bake pass 总耗时(**B 的核心每帧成本**)
    int bakedTiles = 0;     // 本帧烘焙的瓦片数(= capped 可见瓦片数,截上限)
    int64_t bakeBytes = 0;  // B 逐瓦片纹理内存 = N × 定尺²×4(未截上限,反映真实需求)
    bool ready = false;
};

class TileCompositeBakePoc {
public:
    TileCompositeBakePoc() = default;
    ~TileCompositeBakePoc() = default;

    TileCompositeBakePoc(const TileCompositeBakePoc&) = delete;
    TileCompositeBakePoc& operator=(const TileCompositeBakePoc&) = delete;

    bool initialize(RenderDevice* device,
                    const TileCompositeBakePocConfig& config);
    /// 惰性建 bake FBO(定尺,复用,逐瓦片重绑)。失败返回 false。
    bool ensureResources();
    /// 跑一帧:对 cappedTileCount 个瓦片各做一次离屏 bake pass,量总耗时。
    TileCompositeBakeFrameStats tick(int cappedTileCount);
    void dispose();

    bool isReady() const {
        return device_ != nullptr && bakeFbo_ != nullptr;
    }
    const TileCompositeBakeFrameStats& lastStats() const { return lastStats_; }
    const TileCompositeBakePocConfig& config() const { return config_; }

private:
    RenderDevice* device_ = nullptr;
    TileCompositeBakePocConfig config_;
    std::unique_ptr<Framebuffer> bakeFbo_;
    // 合成资源(GLES;Metal shader 未接线时为空 → quadsPerTile 退化为只 pass 切换)。
    std::unique_ptr<ShaderProgram> compositeShader_;
    std::unique_ptr<Buffer> quadBuffer_;
    std::unique_ptr<Texture> sourceTexture_;  // 代表被合成的覆盖影像源
    TileCompositeBakeFrameStats lastStats_;
};

}  // namespace earth_engine
