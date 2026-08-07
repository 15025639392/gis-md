#pragma once

#include <cstdint>

namespace earth_engine {
namespace contracts {

/// 层间契约的集中登记处 —— 单一事实来源。
///
/// 背景:本引擎占比最大的一类根因不在某个模块内部,而在**两层之间的约定不一致**
/// (nodata 哨兵、V 轴方向、绘制次序、档位配对)。这类错误只在最终画面显形,于是
/// 每次都退化成"改一版跑一版"的端到端 A/B,而真正的答案在制造它的那一层。本文件
/// 让违约在那一层就变成可数信号。
///
/// 与 EnvSnapshot.h 的分工:环境快照答"这次跑在什么条件下"(树外因素),契约答
/// "哪一层交出了不符合下游假设的东西"(层间的边)。两者都不管节点内部逻辑。
///
/// 三条刻意的设计选择:
///   1. **不 abort**。管线里大量违约是暂态的(加载中、重镶间隙),崩掉没法用,
///      真机上更不能崩。违约是信号不是断言。
///   2. **release 下保留**。事故全部发生在真机 release —— debug-only 的断言等于
///      没有。开销是一个可预测分支 + 一次 relaxed 递增。
///   3. **首次违约打详细上下文,之后只计数**。违约常常每帧数千次,全打会淹掉
///      logcat 并自己改变时序(诊断把被诊断的现象改了)。
///
/// 新增契约必须同时满足:
///   - 契约陈述是一句**可判真假**的话。反例:「高度纹理正确」;正例:「本次绘制
///     的模板档 == 纹理档」。写不出可判的句子 = 这条边还没想清楚,先别写代码。
///   - 判定点在**消费侧**。只有消费者知道自己假设了什么;写在生产侧容易退化成把
///     生产逻辑重实现一遍,那样两边一起错时检查照样通过。
///   - 谓词与被检查的逻辑**数据独立**。理想形态是拿两条独立的数据流互相印证
///     (例:顶点纬度 vs 顶点 v 坐标),而不是重算一遍生产者刚算过的量。
///   - 在 kOwners 里登记**两端**(见 Owners):判定点只是发现的人,该改的通常是
///     生产侧。责任查表得出,不靠出事当场判断。
///
/// ⚠️ **放宽一条契约,和收紧它同等重大。**
/// 责任指向一旦清晰,就出现一种反向激励:把契约改松比修 bug 便宜。而放松是**静默
/// 的** —— coverage 照常增长、违约数归零,看起来比修好了还干净,比 coverage=0 更难
/// 发现。所以本文件与各判定点的 diff 应当在 review 里被单独看待,与改测试断言同级:
/// 谓词变弱、阈值变松、跳过条件变多,都要有和当初加它时同等分量的理由。
///
/// 编译期能锁死的不进这里 —— 见 BackendWindingContract.h / GltfUniformBlock.h。
enum class Id : uint8_t {
    /// 数据源 → 采样:解码出的高度图,其 nodata 哨兵表能拦住该源的 nodata 底值。
    /// 违约现象:数据空洞/重叠环被当合法高度混进边缘双线性 → km 级假深沟(顶点
    /// 被紧 near/far 裁掉 = 俯视黑裂缝)与假悬崖法线(瓦片边界光照条带)。
    DemNodataSentinel,

    /// 网格构建 → UV 消费:地形网格纹理坐标遵循 NW 约定(v=0 在北)。
    /// 违约现象:上采样按 NW 切子窗,拿到 SE 约定的父网格会上下翻,影像贴反。
    /// 该约定目前由 6 处独立注释各自声明,形态与 winding 收归前完全一致。
    TexcoordNwOrigin,

    /// 分组 → 实例流:同一实例化批内的全部实例携带同一位移模板栅格边长。
    /// 批按位移模板 VBO 指针分组,而模板按 {schemeId,z,row,gridSize} 缓存 ——
    /// "同批必然同档"是**分组规则的副产物**,不是被谁强制的。分组规则一旦改动
    /// (例如按别的键合并),批内混档会让 shader 按错误的栅格边长解算格点位置,
    /// 地形几何整批错位,且没有任何报错。
    BatchTemplateGridParity,

    /// 叠画方 → 页存储:decoratePage 之前,本帧的 tickDecorator 必须已经跑过。
    /// 叠画方在 tickDecorator 里把网格传上 GPU,decoratePage 才是真 draw。次序
    /// 反了会画到上一帧的(或空的)网格上。E4 那次的根因同源:叠加层写入必须晚于
    /// 页存储合成,否则被覆盖 —— 时序契约在渲染管线里格外容易被重排悄悄破坏。
    PageDecorateOrdering,

    // ---- 以下三条来自 AI_INDEX.md §20「Cross-Subsystem Contracts」----
    // 那一节的小标题自己写着 "enforced by call order, not by types" —— 架构文档
    // **声明了**这些契约,但它们只靠调用顺序维持。上面四条边是从事故史挑的
    // (幸存者偏差:只覆盖已经炸过的地方),这三条来自**设计意图**,两个来源都要有
    // 覆盖,否则"架构是否被忠实执行"这个问题根本没有可查的答案。

    /// 渲染 → 资源释放:releaseRenderReferences 必须晚于本帧的 submit。
    /// 渲染命令持有**裸** Buffer*/Texture* 加 resourceKeepAlive shared_ptr,
    /// 先释放会在 draw 中途释放 GPU 资源。文档 §20「submit BEFORE
    /// releaseRenderReferences」。
    SubmitBeforeReleaseRefs,

    /// 加载 → 上传:drainGpuUploadQueue 必须晚于本帧的 processPendingLoads。
    /// worker 解码由前者派发、推进 GpuUploadQueue,同帧的 drain 才取得到;
    /// 反过来上传恒滞后一帧。文档 §20「processPendingLoads BEFORE
    /// drainGpuUploadQueue」。
    LoadsBeforeGpuDrain,

    /// worker → 主线程:GpuUploadQueue 严格 FIFO。
    /// 谓词拿**独立维护的出队序号**与入队时打上的序号比对 —— 与队列实现无关,
    /// 换 push_front / 加优先级重排都会当场被抓。文档 §20「Worker → main
    /// GpuUploadQueue FIFO」。
    GpuUploadQueueFifo,

    /// 在飞请求追踪的三张表(键集/内容键集/取消令牌表)同步变异。
    /// 与 PageDecorateOrdering 同类:模块内不变量,如实登记不编假生产方。
    /// 违约的静默后果分两个方向,都不报错:键残留而令牌先没 → 该 cacheKey
    /// 永判"在飞"→ 调度器永不重发 → 瓦片永久缺失;令牌残留而键先没 →
    /// 令牌表泄漏 + 换代取消形同虚设。三张表今天由同一组方法成对增删,
    /// 契约防的是未来某次改动只动其中一张。
    PendingRequestParity,

    Count
};

/// 供日志用的稳定短名(= 枚举名)。
const char* name(Id id);

/// 一条边的存在条件。
///
/// 为什么需要:coverage=0 有两种完全不同的含义 —— 「该跑却没跑到」(判定点失效/
/// 路径已死,**需要处理**)与「这条路被配置关掉了」(预期之内,**不需要处理**)。
/// 混成一个 dead 计数,就会出现一条永远亮着的警告;而按本文件自己的准入标准,
/// 常亮警告会训练出「这条可以忽略」的习惯,进而一并废掉旁边真正有效的契约。
///
/// 实例:TexcoordNwOrigin 的判定点在 GltfTerrainUpsampler::upsampleForRasterOverlay,
/// 而 decoupleImageryFromGeometry=true(生产默认)下影像不再驱动几何细化,该函数
/// 根本不被调用。真机实测:decouple=true → coverage 0;decouple=false → 152 次
/// 求值、零违约。判定点是好的,只是那条路被关了。
enum class Gate : uint8_t {
    /// 无条件应当执行 —— coverage=0 一律算 dead。
    Always = 0,
    /// 影像驱动的几何上采样路径,仅 decoupleImageryFromGeometry=false 时存在。
    ImageryDrivenUpsample,
    Count
};

Gate gate(Id id);
const char* gateName(Gate g);

/// 由配置装载处告知某条闸此刻是否成立(见 EarthEngineSdkFacade::installScene)。
/// **默认全部为 active** —— 宁可多报一条 dead,也不要因为没人登记而静默吞掉
/// 「该跑却没跑」。
void setGateActive(Gate g, bool active);
bool gateActive(Gate g);

/// 一条边的两端。
///
/// 为什么必须两端都记:层间契约的定义就是**消费侧检测、生产侧制造**。判定点写在
/// 消费侧是对的(只有消费者知道自己假设了什么),但代价是违约日志天然指向那个
/// **没做错事**的模块。只报判定点,凌晨两点看到这行的人会先去翻错的文件。
///
/// 所以责任是**查表得出的**,不是出事当场临时判断 —— 后者在压力下必然退化成
/// "谁最近改过谁背"。
struct Owners {
    /// 交出了不符合下游假设的东西的那一方 = **通常该改的人**。
    const char* producer;
    /// 判定点所在 = 发现的人。coverage=0 时也是"谁的判定点没跑到"。
    const char* consumer;
};
Owners owners(Id id);

/// 记一次**求值**(无论通过与否)。GE_CONTRACT 每次执行都调用。
///
/// 为什么要单独数通过的次数:一条**永远不会触发**的契约,和一条**每次都通过**的
/// 契约,只看违约数是完全一样的(都是 0)。没有求值计数,就无法区分"这条边确实
/// 成立"与"这条边的判定点根本没跑到"——而后者正是契约悄悄变成死代码的方式。
/// coverage=0 是一个**需要处理的信号**:要么路径已死,要么当前场景没覆盖到它。
///
/// 这也是外部驱动困难的边(需要 GL 上下文/完整设备夹具才能构造反例的那些)唯一
/// 现实的活性证据:真机上 coverage 持续增长 = 判定点确实在执行。
void recordEvaluation(Id id);

/// 记一次违约。首次(进程内,按 id)额外打一条带上下文的 Warning。
///
/// 线程安全:计数走 relaxed 原子 —— 解码/镶嵌都在 worker 池上,违约可能来自任意
/// 线程。只关心数量级,不需要顺序保证。
void recordViolation(Id id, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/// 帧末汇总一行(tag="Contract")。全绿时**不打**,故稳态零日志量 —— 这一行只在
/// 出问题时出现,出现即定位。
///
/// 每 id 报"本帧计数(累计计数)":逐帧清零的那个数区分暂态与稳态违约。
void logFrameSummary(uint64_t frameId);

/// 覆盖行:报每条边被求值了多少次(tag="Contract")。与违约汇总分开,因为它**总是**
/// 要打 —— 全绿时它正是唯一能证明契约还活着的东西。
///
/// coverage=0 且**闸成立**的边才算 dead(升 Warning + 单独点名到判定点所在):
/// 判定点没跑到,这条契约此刻等于不存在。闸不成立的边报 disabled,不升级 ——
/// 它没跑是预期之内,不该占用"需要处理"这个信号。
void logCoverage(uint64_t frameId);

/// 累计违约数(测试/自检用;0 = 该契约自进程启动从未被违反)。
uint32_t totalViolations(Id id);

/// 累计求值数(测试/自检用;0 = 该契约的判定点从未执行过 = 它是死的)。
uint32_t totalEvaluations(Id id);

}  // namespace contracts
}  // namespace earth_engine

/// 消费侧判定点。`cond` 为真 = 契约成立。
///
/// 用法:GE_CONTRACT(contracts::Id::Xxx, a == b, "a=%d b=%d", a, b);
/// 上下文格式串要带上**足够复现的量**(瓦片 key、档位、实际值),首违约那一条是
/// 你唯一能拿到的现场。
///
/// 格式串并进 __VA_ARGS__(而不是单列一个 fmt 形参 + `##__VA_ARGS__`)是为了避开
/// GNU 的逗号吞并扩展:无附加实参时 `##` 会触发
/// -Wgnu-zero-variadic-macro-arguments,而 __VA_OPT__ 要 C++20,本工程是 C++17。
/// 这样至少有一个变参(格式串本身),两边都不用妥协。
///
/// `cond` 只求值一次 —— 带副作用的条件重复求值会静默改变被诊断的行为。
///
/// 每次执行都记一次求值(见 recordEvaluation:没有它就分不清"边成立"与"判定点
/// 没跑到")。代价是通过路径上多一次 relaxed 原子加,因此判定点不要放进逐像素/
/// 逐顶点级别的循环 —— 契约是层间的边,那个粒度上本来也不该有边。
#define GE_CONTRACT(id, cond, ...)                                         \
    do {                                                                   \
        ::earth_engine::contracts::recordEvaluation((id));                 \
        if (!(cond)) {                                                     \
            ::earth_engine::contracts::recordViolation((id), __VA_ARGS__); \
        }                                                                  \
    } while (0)
