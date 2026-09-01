#include "IconAtlas.h"

#include "../debug/PlatformLog.h"
#include "RenderDevice.h"

#include <unordered_map>

namespace earth_engine {

struct IconAtlas::Impl {
    RenderDevice* device = nullptr;
    std::unique_ptr<Texture> texture;
    // shelf 打包游标(与 GlyphAtlas 同策略:行内排,行满换行,页满失败)
    int cursorX = 0;
    int cursorY = 0;
    int rowHeight = 0;
    uint64_t revision = 0;
    std::unordered_map<std::string, Frame> frames;
    // 页满拒绝计数(见 IconAtlas::pageFullRejectCount)。
    int pageFullRejects = 0;
};

IconAtlas::IconAtlas(RenderDevice* device) : impl_(std::make_unique<Impl>()) {
    impl_->device = device;
}

IconAtlas::~IconAtlas() = default;

bool IconAtlas::addImage(const std::string& name,
                         int width,
                         int height,
                         const std::vector<uint8_t>& rgba) {
    if (!impl_->device || name.empty() || width <= 0 || height <= 0) {
        return false;
    }
    if (width > kAtlasSize || height > kAtlasSize) return false;
    if (rgba.size() != static_cast<size_t>(width) * height * 4u) return false;

    if (!impl_->texture) {
        // RGBA8 单页,线性过滤无 mip(图标屏幕尺寸范围有限;mip 链对增量
        // region 上传不友好,与 GlyphAtlas 同约定)。
        TextureDesc desc;
        desc.width = kAtlasSize;
        desc.height = kAtlasSize;
        desc.format = TextureDesc::Format::RGBA8;
        desc.data = nullptr;
        desc.mipmap = false;
        desc.minFilter = TextureDesc::Filter::Linear;
        desc.magFilter = TextureDesc::Filter::Linear;
        impl_->texture = impl_->device->createTexture(desc);
        if (!impl_->texture) return false;
    }

    const int cellW = width + kPadding;
    const int cellH = height + kPadding;
    if (impl_->cursorX + cellW > kAtlasSize) {
        impl_->cursorX = 0;
        impl_->cursorY += impl_->rowHeight;
        impl_->rowHeight = 0;
    }
    if (impl_->cursorY + cellH > kAtlasSize) {
        // 页满(无淘汰):计数 + 首次告警,不允许静默丢图标。
        if (impl_->pageFullRejects++ == 0) {
            platformLog(LogLevel::Warning, "IconAtlas",
                        "atlas page full — further icons rejected "
                        "(check pageFullRejectCount)");
        }
        return false;
    }

    const int px = impl_->cursorX;
    const int py = impl_->cursorY;
    impl_->cursorX += cellW;
    if (cellH > impl_->rowHeight) impl_->rowHeight = cellH;

    if (!impl_->device->updateTextureRegion(
            impl_->texture.get(), px, py, width, height, rgba.data(),
            static_cast<size_t>(width) * 4u)) {
        return false;
    }

    const float inv = 1.0f / static_cast<float>(kAtlasSize);
    Frame f;
    f.u0 = static_cast<float>(px) * inv;
    f.v0 = static_cast<float>(py) * inv;
    f.u1 = static_cast<float>(px + width) * inv;
    f.v1 = static_cast<float>(py + height) * inv;
    f.widthPx = static_cast<float>(width);
    f.heightPx = static_cast<float>(height);
    impl_->frames[name] = f;
    ++impl_->revision;
    return true;
}

const IconAtlas::Frame* IconAtlas::frame(const std::string& name) const {
    auto it = impl_->frames.find(name);
    return it == impl_->frames.end() ? nullptr : &it->second;
}

void IconAtlas::removeNamePrefix(const std::string& prefix) {
    if (prefix.empty()) return;
    bool removed = false;
    for (auto it = impl_->frames.begin(); it != impl_->frames.end();) {
        if (it->first.rfind(prefix, 0) == 0) {
            it = impl_->frames.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (removed) ++impl_->revision;
}

bool IconAtlas::empty() const { return impl_->frames.empty(); }

int IconAtlas::pageFullRejectCount() const { return impl_->pageFullRejects; }

Texture* IconAtlas::texture() { return impl_->texture.get(); }

const Texture* IconAtlas::texture() const { return impl_->texture.get(); }

uint64_t IconAtlas::revision() const { return impl_->revision; }

} // namespace earth_engine
