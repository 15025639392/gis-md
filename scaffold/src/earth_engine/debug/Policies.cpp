#include "Policies.h"

#include "PlatformLog.h"

#include <atomic>
#include <cstdio>

namespace earth_engine {
namespace policy {
namespace {

constexpr size_t kCount = static_cast<size_t>(Id::Count);

// 窗口内累计。报表输出后清零 —— 不清零的话报的是"自启动以来的平均",冷启动那
// 几百帧会永久拖住数字,而 A/B 里最怕的正是把陈旧值当当前值读。
std::atomic<uint64_t> windowNum_[kCount];
std::atomic<uint64_t> windowDen_[kCount];

const char* const kNames[] = {
    "BatchFormation",
    "PageResidency",
    "CellPageCoverage",
    "IndirLayerAllocNoEvict",
    "FinalizeProgress",
    "HeightIndexRegularity",
    "HeightSampleCoverage",
    "TerminalStateProgress",
    "RasterUploadProgress",
};

const Expectation kExpectations[] = {
    // 合批率。有资格的命令**按定义**就是"能合的",资格闸已经把不能合的挡在外面
    // 了,所以剩下的应当绝大多数进批;掉下来只可能是分组太碎(每组不足 2 个)。
    // 下界取 0.5 而非 0.9:单例组是正常现象(孤立瓦片、边缘 {z,row}),留出余量。
    {0.5, 1.0,
     "有资格命令按定义可合批(资格闸已挡掉不可合的);低于此说明分组过碎 —— "
     "真机拉远相机实测 20/20=1.00,默认相机 2/3。恒 0 = 合批完全空转,"
     "曾因资格闸事实上不可达而长期如此且无人察觉",
     "TerrainInstanceBatcher::assemble + TerrainPageStore 资格闸"},

    // 页驻留率。合批资格 = 该瓦片所有会产生片元的 cell 都有页。稳态下可见瓦片
    // 应当基本都达标;长期偏低说明页 fetch 跟不上或资格闸又变得不可达。
    // 下界 0.5:加载期本来就会低,窗口聚合已摊掉大部分暂态。
    {0.5, 1.0,
     "稳态下可见瓦片应基本都达到全 cell 驻留;偏低 = 页 fetch 跟不上,或资格闸"
     "定义再次变得不可达(旧闸要 gridN²=1024 全驻留而全局仅 ~52 页,恒为 0)",
     "TerrainPageStore::updateVisiblePages"},

    // cell 页覆盖率。差额全部回落 mappedRaster(祖先影像)= 糊。下界 0.6 而非更高:
    // SSE 地板本来就会**故意**剔掉远景/掠射 cell,那部分回落是设计意图不是故障;
    // 真正要抓的是它整体塌下去(页 fetch 跟不上、层池不够、源不可达)。
    {0.6, 1.0,
     "会产生片元的 cell 应多数拿到高清页;差额回落 mappedRaster 即观感变糊。"
     "SSE 地板故意剔远景 cell,故下界留到 0.6 —— 抓的是整体塌陷不是正常剔除",
     "TerrainPageStore::updateVisiblePages(页 fetch / 层池容量)"},

    // 间接层无换租获取率。稳态应接近 1:可见集稳定时不该反复互相踢。
    // 持续偏低 = 层池容量装不下当前可见集(thrash),表现为闪烁与重复上传。
    {0.9, 1.0,
     "稳态可见集不该反复互相淘汰;持续偏低 = 层池容量不足以承载当前可见集"
     "(thrash),表现为闪烁/重传。相机大幅移动时短暂下探属正常,窗口聚合已摊薄",
     "TerrainPageStore 层池容量 Config::maxPages"},

    // finalize 推进率。只在"有活可做"的帧上记账,故健康态应≈1:有活就该推进至少
    // 一个。下界 0.8 留给时间预算耗尽的偶发帧;持续趋 0 = 通路冻结。
    {0.8, 1.0,
     "只在进 finalize 循环时本来就有活的帧上记账,故健康态应≈1(有活就该推进"
     "至少一个)。下界 0.8 留给时间预算耗尽的偶发帧。持续趋 0 = 通路冻结 —— "
     "历史事故:交互期 Urgent-only 硬冻结,早退发生在 tryFinalize 之前",
     "TilePendingLoadProcessor::processPendingLoads / 预算配置"},

    // 高度索引正规率。生产不变量:地形瓦 bounds 恒等于 scheme 矩形
    // (HeightmapTerrainContentProvider 的"显式 bounds"就是 tileToRectangle
    // 本身;虚拟根 MAXIMUM bounds 不携带 heightmap)——故应精确 1.0。
    // <1 说明 bounds 毒化复发或新 provider 破坏不变量:正确性由溢出列表兜住,
    // 但每个溢出瓦都在把 TerrainHeightService 点查询悄悄退化回线性扫,
    // 正是"单点全对、整体退化"型静默故障。
    {1.0, 1.0,
     "地形瓦 bounds==scheme矩形是已根修的生产不变量(上采样 bounds 毒化那轮),"
     "provider 显式 bounds 即 tileToRectangle 本身,故恒 1.0;<1 = 不变量被"
     "破坏,点查询按溢出瓦数量退化向旧全表扫描",
     "TerrainHeightService::rebuild(boundsMatchScheme)/ 各地形 provider"},

    // 高度采样命中率。全球地形源全覆盖 + 在视祖先受选择遍历的淘汰豁免
    // (active set 逐帧 markIneligibleForUnloading),miss 只应来自"补货侧"
    // 暂态:kicked 深降跳过中间档加载、冷启动首窗、离视归来。下界 0.8 与
    // FinalizeProgress 同理留给加载窗口;**持续**偏低 = 回退链断
    // (祖先 heightmap 长期缺位),是"加载期破洞/矢量贴海平面"类可见瑕疵
    // 的前兆。分母 0(高空早退无查询)不参与判定。
    {0.8, 1.0,
     "全球源全覆盖+在视祖先淘汰豁免下 miss 只应来自深跳/冷启动补货暂态,"
     "窗口聚合已摊薄;持续偏低=祖先回退链断,fill 贴海平面/矢量贴 0 的前兆",
     "TerrainHeightService 消费方加载时序 / TileSelection kicked 深降路径"},

    // terminal 推进率。与 FinalizeProgress 同构同区间:只在有终态积压的帧上
    // 记账,有活就该推进至少一个。下界 0.8 留给预算耗尽的偶发帧。
    {0.8, 1.0,
     "只在有终态积压的帧上记账,健康态应≈1。FinalizeProgress 的镜像:同一"
     "冻结事故形态(早退/预算把 lane 闸死)对 terminal 循环同样成立,此前"
     "只有下半段 finalize 有守卫",
     "TilePendingLoadProcessor::processPendingLoads(terminal 循环)/ 预算配置"},

    // 影像上传推进率。分母只数"存在符合交互期资格的积压"的帧 —— 交互期大图
    // 被尺寸过滤推迟是设计意图,不入分母,冻结与设计内推迟从此分得开。
    {0.8, 1.0,
     "只在存在能过交互期尺寸过滤的待上传项的帧上记账,健康态应≈1。持续趋 0 "
     "= RasterTextureUpload lane 冻结 —— 影像侧正是交互期硬冻结改 budget "
     "涓流那次修复的先行现场,守卫防它复发",
     "RasterOverlayTileProvider::processPendingUploads / RasterTextureUpload lane"},
};

// ⚠️ 数组**不写显式尺寸**,让初始化项数决定长度 —— 否则 `kNames[kCount]` 的
// sizeof 恒等于 kCount,下面这条 static_assert 就是同义反复、一个都拦不住。
// 曾因此漏掉一条 expectation:真机报 owner=(null) 依据=(null),区间读成 [0,0],
// 而编译期一声不吭。守卫本身也需要被验证,这条就是代价。
static_assert(sizeof(kNames) / sizeof(kNames[0]) == kCount,
              "policy::Id 与 kNames 必须逐项对应。");
static_assert(sizeof(kExpectations) / sizeof(kExpectations[0]) == kCount,
              "policy::Id 与 kExpectations 必须逐项对应:新增策略必须给出区间"
              "**和依据**,写不出依据就先别加。");

}  // namespace

const char* name(Id id) {
    const size_t index = static_cast<size_t>(id);
    return index < kCount ? kNames[index] : "?";
}

Expectation expectation(Id id) {
    const size_t index = static_cast<size_t>(id);
    return index < kCount ? kExpectations[index]
                          : Expectation{0.0, 1.0, "?", "?"};
}

void observe(Id id, int numerator, int denominator) {
    const size_t index = static_cast<size_t>(id);
    if (index >= kCount || denominator <= 0) return;  // 无机会不计
    windowNum_[index].fetch_add(static_cast<uint64_t>(numerator < 0 ? 0
                                                                    : numerator),
                                std::memory_order_relaxed);
    windowDen_[index].fetch_add(static_cast<uint64_t>(denominator),
                                std::memory_order_relaxed);
}

uint64_t windowNumerator(Id id) {
    const size_t index = static_cast<size_t>(id);
    return index < kCount ? windowNum_[index].load(std::memory_order_relaxed)
                          : 0;
}

uint64_t windowDenominator(Id id) {
    const size_t index = static_cast<size_t>(id);
    return index < kCount ? windowDen_[index].load(std::memory_order_relaxed)
                          : 0;
}

void logReport(uint64_t frameId) {
    char line[512];
    int offset = 0;
    int outOfRange = 0;
    double ratios[kCount] = {};
    bool hadSamples[kCount] = {};

    for (size_t i = 0; i < kCount; ++i) {
        const uint64_t num = windowNum_[i].exchange(0, std::memory_order_relaxed);
        const uint64_t den = windowDen_[i].exchange(0, std::memory_order_relaxed);
        hadSamples[i] = den > 0;
        ratios[i] = den > 0 ? static_cast<double>(num) / static_cast<double>(den)
                            : 0.0;
        const bool out = hadSamples[i] && (ratios[i] < kExpectations[i].lo ||
                                           ratios[i] > kExpectations[i].hi);
        if (out) ++outOfRange;
        const int written = std::snprintf(
            line + offset, sizeof(line) - static_cast<size_t>(offset),
            "%s%s=%s%.2f[%.2f-%.2f]%s", offset > 0 ? " " : "", kNames[i],
            hadSamples[i] ? "" : "n/a:",
            ratios[i], kExpectations[i].lo, kExpectations[i].hi,
            out ? "!OUT" : "");
        if (written <= 0 ||
            static_cast<size_t>(offset + written) >= sizeof(line)) {
            break;
        }
        offset += written;
    }

    // 总是打:全部在区间内时,这一行正是唯一能证明策略还在按预期运行的东西。
    // 只有越界才升 Warning(同契约 coverage 的分级)。
    platformLog(outOfRange > 0 ? LogLevel::Warning : LogLevel::Info, "Policy",
                "f=%llu out=%d %s",
                static_cast<unsigned long long>(frameId), outOfRange, line);

    // 越界逐条点名,带上区间依据与归属 —— 这行是要拿去派活的,而且看的人得能判断
    // 是系统坏了还是这个区间当初就定错了。
    for (size_t i = 0; i < kCount; ++i) {
        if (!hadSamples[i]) continue;
        if (ratios[i] >= kExpectations[i].lo &&
            ratios[i] <= kExpectations[i].hi) {
            continue;
        }
        platformLog(LogLevel::Warning, "Policy",
                    "  OUT-OF-RANGE %s=%.3f 期望[%.2f-%.2f] owner=%s | 依据:%s",
                    kNames[i], ratios[i], kExpectations[i].lo,
                    kExpectations[i].hi, kExpectations[i].owner,
                    kExpectations[i].rationale);
    }
}

}  // namespace policy
}  // namespace earth_engine
