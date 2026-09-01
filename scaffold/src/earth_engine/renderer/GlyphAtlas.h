#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace earth_engine {

class RenderDevice;
class Texture;

/// SDF 字形图集(矢量 P5b 文字标注)。
///
/// 字体字节由应用层注入(setFontData,引擎不读文件系统——分层原则);
/// stb_truetype 按需栅格化 SDF 字形,shelf 打包进 R8 texture array
/// (SDF 原生单通道；数组 region 使用后端的异步上传路径),codepoint →
/// {uv,布局 metrics} 缓存。渲染线程调用(纹理上传需 GL 上下文)。
///
/// 限度:单页 2048² 图集无淘汰,32px 栅格约容纳 ~4096 字形。容量按底图
/// 数据实测定(符号刀B):重庆 POI name 字符集 = 1036 唯一字符,原
/// 1024²/48px 只容 ~270 必爆,2048²/32px 留 ~4× 余量(32px SDF 对
/// labelSizePx≈28 无损)。跨城市漫游字形集单调增长,页满看
/// atlasFullDropCount 报警再上多页/LRU,不预支复杂度。stb_truetype
/// 只支持 TrueType glyf 轮廓(CFF/OTF PostScript 不支持,Android 的
/// NotoSansCJK .ttc 是 CFF —— 字体探测归应用层)。
class GlyphAtlas {
public:
    static constexpr int kAtlasSize = 2048;
    static constexpr int kGlyphPixelHeight = 32;  ///< 栅格化像素高
    static constexpr int kSdfPadding = 6;         ///< SDF 外扩(px)
    static constexpr unsigned char kSdfOnEdge = 180;  ///< 轮廓处 SDF 值
    static constexpr float kSdfDistScale = 24.0f;     ///< SDF 值/px
    static constexpr int kAmapProviderPixelHeight = 24;
    static constexpr unsigned char kAmapSdfOnEdge = 205;

    explicit GlyphAtlas(RenderDevice* device);
    ~GlyphAtlas();

    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;

    /// 注入字体字节(TrueType/ttc 首字体)。失败(解析不了/CFF)返回 false。
    bool setFontData(std::vector<uint8_t> fontData);
    /// Invalidates the current font and every derived glyph registration.
    void clearFontData();
    bool ready() const;
    float metricPixelHeight() const;

    /// 单字形:uv + 布局盒(px,基线原点,y 向上为正)。
    struct Glyph {
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;  ///< 图集 uv([0,1])
        float offsetX = 0;   ///< 位图左缘相对 pen x
        float offsetY = 0;   ///< 位图上缘相对基线(y 向上,正=基线上方)
        float width = 0;     ///< 位图宽 px
        float height = 0;    ///< 位图高 px
        float advance = 0;   ///< pen 前进 px
        bool hasBitmap = false;  ///< 空白字符 false(只前进不出图)
    };
    struct ProviderGlyph {
        uint32_t codepoint = 0;
        int fontWidth = 0;
        int fontHeight = 0;
        int horiBearingX = 0;
        int horiBearingY = 0;
        int horiAdvance = 0;
        int posX = 0;
        int posY = 0;
    };

    /// 取字形(缺则栅格化+上传)。失败(无字体/无字形/图集满)返回 nullptr。
    const Glyph* ensureGlyph(uint32_t codepoint);

    /// 渲染帧共享的动态字形预算。GlyphAtlas 是 Renderer 级单例，故预算
    /// 在这里按 frameId 只重置一次；多个 FeatureRenderLayer 不能各自获得
    /// 一份 4ms。预算耗尽后仍允许本帧的第一个缺字形启动，避免单字形成本
    /// 本身超过预算时永久饥饿；一旦启动过，后续缺字形延迟到下一帧。
    void beginFrameGlyphBudget(uint64_t frameId, double budgetMs);
    enum class BudgetedGlyphResult {
        Ready,
        MissingTerminal,
        Deferred,
        Saturated
    };
    BudgetedGlyphResult ensureGlyphBudgeted(uint32_t codepoint);
    /// 结束当前调用方的字形派发批次。Android 会封口一张有界 batch
    /// Landing：整批 worker 结果全部入箱后只唤醒一次；其他平台 no-op。
    void finishGlyphRasterDispatch();
    size_t frameGlyphRasterAttempts() const;
    double frameGlyphRasterMs() const;

    /// 未烘标签是否还需要一帧来重试派发字形。Android 本帧只要已成功派发
    /// 或已有任务在途，后续进展都由 worker 自行完成，应等待 Landing 唤醒；
    /// 仅“零在途且本帧一次也没派成”（try_lock 竞争等）时保持 Pumped，
    /// 防止无人再触发重试。非 Android 同步路径始终返回 true。
    bool needsFrameForGlyphRasterDispatch() const;

    /// Android 后台正在栅格化/等待回收的字形数（诊断与门控）；其他平台 0。
    size_t pendingGlyphRasterCount() const;

    /// 已驻留字形数(诊断:新字形栅格化是渲染线程上的同步 SDF 计算,
    /// 平移进新区域时成批发生 —— P6 慢帧的真因,量它才能定预算)。
    size_t residentGlyphCount() const;
    /// 字形是否已在图集内(不触发栅格化)。预算调度用。
    bool hasGlyph(uint32_t codepoint) const;
#if defined(EARTH_ENGINE_TESTING)
    void activateAmapOfficialProviderForTest(
        std::function<void(uint32_t)> demand) {
        activateAmapOfficialProvider(std::move(demand));
    }
    bool installAmapOfficialGlyphBatchForTest(
        int imageWidth, int imageHeight,
        const std::vector<uint8_t>& grayscale,
        const std::vector<ProviderGlyph>& glyphs) {
        return installAmapOfficialGlyphBatch(imageWidth, imageHeight,
                                             grayscale, glyphs);
    }
#endif

    /// 图集页满导致的字形丢弃计数(ensureGlyph 因页满返回 nullptr 的次数)。
    /// >0 = 屏幕上有字渲染不出来。静默丢字不可接受:首次溢出打 Warning
    /// 日志,消费方(诊断面板/测试)据此报警。多页/LRU 落地前的可观测兜底。
    int atlasFullDropCount() const;
    /// shelf 已用高度(px)。/ kAtlasSize 即填充率 —— 图集满不是内存问题
    /// (纹理定长 16MB),是**永久丢字**:ensureGlyph 返回 nullptr 且无淘汰。
    /// 故水位必须能被观测,不能只等 atlasFullDropCount 报第一次。
    int shelfUsedHeightPx() const;

    /// 字体行度量(px,kGlyphPixelHeight 尺度)。
    float ascent() const;
    float descent() const;

    /// 图集纹理(未初始化返回 nullptr)。
    Texture* texture();
    const Texture* texture() const;

    /// 成功注入字体的代次。字体 ready→ready 替换会清空并重排图集，
    /// 消费方据此失效已经烘焙的 UV/标签派生数据；失败注入不递增。
    uint64_t revision() const;

    /// UTF-8 → codepoints(静态工具;非法序列按字节回退,不丢文本)。
    static std::vector<uint32_t> decodeUtf8(const std::string& text);

private:
    friend class Engine;
    friend class AmapClassicAssets;
    void activateAmapOfficialProvider(
        std::function<void(uint32_t)> demand);
    bool installAmapOfficialGlyphBatch(
        int imageWidth, int imageHeight,
        const std::vector<uint8_t>& grayscale,
        const std::vector<ProviderGlyph>& glyphs);
    void clearAmapOfficialProvider();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace earth_engine
