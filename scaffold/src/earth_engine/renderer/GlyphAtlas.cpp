#include "GlyphAtlas.h"

#include "../debug/PlatformLog.h"
#include "RenderDevice.h"

#if __has_include(<stb_truetype.h>)
#include <stb_truetype.h>
#define EARTH_ENGINE_HAS_STB_TRUETYPE 1
#else
#define EARTH_ENGINE_HAS_STB_TRUETYPE 0
#endif

#include <cstring>
#include <unordered_map>

namespace earth_engine {

struct GlyphAtlas::Impl {
    RenderDevice* device = nullptr;
    std::vector<uint8_t> fontData;
    bool fontReady = false;
#if EARTH_ENGINE_HAS_STB_TRUETYPE
    stbtt_fontinfo font{};
#endif
    float scale = 0.0f;
    float ascentPx = 0.0f;
    float descentPx = 0.0f;

    std::unique_ptr<Texture> texture;
    // shelf 打包游标
    int cursorX = 0;
    int cursorY = 0;
    int rowHeight = 0;

    std::unordered_map<uint32_t, Glyph> glyphs;
    // 页满丢字计数(见 GlyphAtlas::atlasFullDropCount)。
    int atlasFullDrops = 0;
    bool nearFullWarned = false;
};

GlyphAtlas::GlyphAtlas(RenderDevice* device)
    : impl_(std::make_unique<Impl>()) {
    impl_->device = device;
}

GlyphAtlas::~GlyphAtlas() = default;

bool GlyphAtlas::setFontData(std::vector<uint8_t> fontData) {
#if EARTH_ENGINE_HAS_STB_TRUETYPE
    if (!impl_->device || fontData.empty()) return false;
    impl_->fontData = std::move(fontData);
    const int offset =
        stbtt_GetFontOffsetForIndex(impl_->fontData.data(), 0);
    if (offset < 0 ||
        !stbtt_InitFont(&impl_->font, impl_->fontData.data(), offset)) {
        impl_->fontReady = false;
        return false;
    }
    impl_->scale =
        stbtt_ScaleForPixelHeight(&impl_->font, kGlyphPixelHeight);
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&impl_->font, &ascent, &descent, &lineGap);
    impl_->ascentPx = static_cast<float>(ascent) * impl_->scale;
    impl_->descentPx = static_cast<float>(-descent) * impl_->scale;

    // 图集纹理:RGBA8(updateTextureRegion 契约为 RGBA;SDF 复制四通道,
    // shader 采 .r),线性过滤,无 mip(屏幕字号缩放范围有限,SDF 自带
    // 平滑;mip 链对增量 region 上传也不友好)。
    if (!impl_->texture) {
        TextureDesc desc;
        desc.width = kAtlasSize;
        desc.height = kAtlasSize;
        desc.format = TextureDesc::Format::RGBA8;
        desc.data = nullptr;
        desc.mipmap = false;
        desc.minFilter = TextureDesc::Filter::Linear;
        desc.magFilter = TextureDesc::Filter::Linear;
        impl_->texture = impl_->device->createTexture(desc);
    }
    // 换字体 → 已缓存字形失效重来。
    impl_->glyphs.clear();
    impl_->cursorX = 0;
    impl_->cursorY = 0;
    impl_->rowHeight = 0;
    impl_->fontReady = impl_->texture != nullptr;
    return impl_->fontReady;
#else
    (void)fontData;
    return false;
#endif
}

bool GlyphAtlas::ready() const { return impl_->fontReady; }
int GlyphAtlas::atlasFullDropCount() const { return impl_->atlasFullDrops; }
float GlyphAtlas::ascent() const { return impl_->ascentPx; }
float GlyphAtlas::descent() const { return impl_->descentPx; }
Texture* GlyphAtlas::texture() const { return impl_->texture.get(); }

size_t GlyphAtlas::residentGlyphCount() const { return impl_->glyphs.size(); }

int GlyphAtlas::shelfUsedHeightPx() const {
    return impl_->cursorY + impl_->rowHeight;
}

bool GlyphAtlas::hasGlyph(uint32_t codepoint) const {
    return impl_->glyphs.find(codepoint) != impl_->glyphs.end();
}

const GlyphAtlas::Glyph* GlyphAtlas::ensureGlyph(uint32_t codepoint) {
#if EARTH_ENGINE_HAS_STB_TRUETYPE
    if (!impl_->fontReady) return nullptr;
    auto it = impl_->glyphs.find(codepoint);
    if (it != impl_->glyphs.end()) return &it->second;

    const int glyphIndex =
        stbtt_FindGlyphIndex(&impl_->font, static_cast<int>(codepoint));
    if (glyphIndex == 0 && codepoint != 0x20) return nullptr;  // 字体无此字形

    int advanceUnits = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&impl_->font, glyphIndex, &advanceUnits, &lsb);
    Glyph glyph;
    glyph.advance = static_cast<float>(advanceUnits) * impl_->scale;

    int w = 0, h = 0, xoff = 0, yoff = 0;
    unsigned char* sdf = stbtt_GetGlyphSDF(
        &impl_->font, impl_->scale, glyphIndex, kSdfPadding, kSdfOnEdge,
        kSdfDistScale, &w, &h, &xoff, &yoff);
    if (!sdf || w <= 0 || h <= 0) {
        // 空白字符(空格等):只前进。
        if (sdf) stbtt_FreeSDF(sdf, nullptr);
        glyph.hasBitmap = false;
        auto [ins, _] = impl_->glyphs.emplace(codepoint, glyph);
        return &ins->second;
    }

    // shelf 打包(行满换行;图集满 → 失败,P5b 无淘汰)。
    if (impl_->cursorX + w > kAtlasSize) {
        impl_->cursorX = 0;
        impl_->cursorY += impl_->rowHeight;
        impl_->rowHeight = 0;
    }
    if (impl_->cursorY + h > kAtlasSize) {
        stbtt_FreeSDF(sdf, nullptr);
        // 图集满(多页/LRU 后置):计数 + 首次告警,不允许静默丢字。
        if (impl_->atlasFullDrops++ == 0) {
            platformLog(LogLevel::Warning, "GlyphAtlas",
                        "atlas page full — further glyphs will not render "
                        "(check atlasFullDropCount)");
        }
        return nullptr;
    }
    const int px = impl_->cursorX;
    const int py = impl_->cursorY;
    impl_->cursorX += w;
    if (h > impl_->rowHeight) impl_->rowHeight = h;
    // 逼近告警:满了之后是**永久丢字**(无淘汰,且 appendLabelTextQuads 对
    // 缺字形 `continue` —— 表现是"幸福广场"变"幸广场",比丢整条标签更难
    // 察觉)。只等 atlasFullDropCount 报第一次已经晚了,故在 80% 先喊一声。
    if (!impl_->nearFullWarned &&
        impl_->cursorY + impl_->rowHeight > kAtlasSize * 4 / 5) {
        impl_->nearFullWarned = true;
        platformLog(LogLevel::Warning, "GlyphAtlas",
                    "shelf %d/%d (80%%) with %zu glyphs — 满后新字形永久不"
                    "渲染(缺字非缺标签)。该上多页/LRU 了",
                    impl_->cursorY + impl_->rowHeight, kAtlasSize,
                    impl_->glyphs.size());
    }

    // SDF 复制四通道上传(region 契约 RGBA)。
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        const uint8_t v = sdf[i];
        rgba[i * 4 + 0] = v;
        rgba[i * 4 + 1] = v;
        rgba[i * 4 + 2] = v;
        rgba[i * 4 + 3] = v;
    }
    stbtt_FreeSDF(sdf, nullptr);
    impl_->device->updateTextureRegion(
        impl_->texture.get(), px, py, w, h, rgba.data(),
        static_cast<size_t>(w) * 4u);

    const float inv = 1.0f / static_cast<float>(kAtlasSize);
    glyph.u0 = static_cast<float>(px) * inv;
    glyph.v0 = static_cast<float>(py) * inv;
    glyph.u1 = static_cast<float>(px + w) * inv;
    glyph.v1 = static_cast<float>(py + h) * inv;
    glyph.offsetX = static_cast<float>(xoff);
    glyph.offsetY = static_cast<float>(-yoff);  // stbtt y 向下 → y 向上
    glyph.width = static_cast<float>(w);
    glyph.height = static_cast<float>(h);
    glyph.hasBitmap = true;
    auto [ins, _] = impl_->glyphs.emplace(codepoint, glyph);
    return &ins->second;
#else
    (void)codepoint;
    return nullptr;
#endif
}

std::vector<uint32_t> GlyphAtlas::decodeUtf8(const std::string& text) {
    std::vector<uint32_t> out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const auto b0 = static_cast<uint8_t>(text[i]);
        uint32_t cp = 0;
        size_t len = 1;
        if (b0 < 0x80) {
            cp = b0;
        } else if ((b0 & 0xE0) == 0xC0) {
            cp = b0 & 0x1F;
            len = 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            cp = b0 & 0x0F;
            len = 3;
        } else if ((b0 & 0xF8) == 0xF0) {
            cp = b0 & 0x07;
            len = 4;
        } else {
            out.push_back(b0);  // 非法首字节:按字节回退
            ++i;
            continue;
        }
        if (i + len > text.size()) {
            out.push_back(b0);
            ++i;
            continue;
        }
        bool valid = true;
        for (size_t k = 1; k < len; ++k) {
            const auto bk = static_cast<uint8_t>(text[i + k]);
            if ((bk & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (bk & 0x3F);
        }
        if (!valid) {
            out.push_back(b0);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

} // namespace earth_engine
