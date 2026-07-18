#include "VirtualTexturePoc.h"

#include "../debug/PerfTimer.h"

#include <algorithm>

namespace earth_engine {

namespace {

// 间接纹理里一个 texel 编码物理槽:R=column, G=row, B=1(有效标志), A=255。
// 骨架用 RGBA8(唯一可创建的多通道格式);列/行 ≤255 时无损(atlas 8×8 远够)。
std::array<uint8_t, 4> encodeSlotTexel(const VtAtlasSlot& slot) {
    return {static_cast<uint8_t>(std::clamp(slot.column, 0, 255)),
            static_cast<uint8_t>(std::clamp(slot.row, 0, 255)),
            uint8_t{1},
            uint8_t{255}};
}

}  // namespace

bool VirtualTexturePoc::initialize(RenderDevice* device,
                                   const VirtualTexturePocConfig& config) {
    if (!device) {
        return false;
    }
    device_ = device;
    config_ = config;
    // 防御:非法配置夹到最小可用值,PoC 不崩。
    config_.feedbackDownscale = std::max(1, config_.feedbackDownscale);
    config_.atlasColumns = std::max(1, config_.atlasColumns);
    config_.atlasRows = std::max(1, config_.atlasRows);
    config_.pageSizeTexels = std::max(1, config_.pageSizeTexels);
    config_.indirectionSize = std::max(1, config_.indirectionSize);

    // 物理 atlas 纹理:(列×页边) × (行×页边),RGBA8,不 mipmap(逐页独立)。
    TextureDesc atlasDesc;
    atlasDesc.width = config_.atlasColumns * config_.pageSizeTexels;
    atlasDesc.height = config_.atlasRows * config_.pageSizeTexels;
    atlasDesc.format = TextureDesc::Format::RGBA8;
    atlasDesc.mipmap = false;
    atlasDesc.minFilter = TextureDesc::Filter::Linear;
    atlasDesc.magFilter = TextureDesc::Filter::Linear;
    atlasTexture_ = device_->createTexture(atlasDesc);
    if (!atlasTexture_) {
        dispose();
        return false;
    }

    // 间接纹理:indirectionSize²,RGBA8,nearest(整数槽坐标不能插值)。
    TextureDesc indirDesc;
    indirDesc.width = config_.indirectionSize;
    indirDesc.height = config_.indirectionSize;
    indirDesc.format = TextureDesc::Format::RGBA8;
    indirDesc.mipmap = false;
    indirDesc.minFilter = TextureDesc::Filter::Nearest;
    indirDesc.magFilter = TextureDesc::Filter::Nearest;
    indirectionTexture_ = device_->createTexture(indirDesc);
    if (!indirectionTexture_) {
        dispose();
        return false;
    }
    indirectionCpu_.assign(
        static_cast<size_t>(config_.indirectionSize) *
            static_cast<size_t>(config_.indirectionSize) * 4u,
        uint8_t{0});

    pageTable_ = std::make_unique<VtPageTable>(config_.atlasColumns,
                                               config_.atlasRows);
    return true;
}

bool VirtualTexturePoc::ensureResources(int surfaceWidthPixels,
                                        int surfaceHeightPixels) {
    if (!device_ || surfaceWidthPixels <= 0 || surfaceHeightPixels <= 0) {
        return false;
    }
    const int wantW =
        std::max(1, surfaceWidthPixels / config_.feedbackDownscale);
    const int wantH =
        std::max(1, surfaceHeightPixels / config_.feedbackDownscale);
    if (feedbackFbo_ && feedbackWidth_ == wantW && feedbackHeight_ == wantH) {
        return true;
    }
    // 尺寸变化 → 惰性重建(maplibre 式)。
    FramebufferDesc desc;
    desc.width = wantW;
    desc.height = wantH;
    desc.hasColor = true;
    desc.hasDepth = true;  // feedback 需深度测试挑最近页(shader 子步用)
    desc.depthSampleable = false;
    auto fbo = device_->createFramebuffer(desc);
    if (!fbo) {
        return false;
    }
    feedbackFbo_ = std::move(fbo);
    feedbackWidth_ = wantW;
    feedbackHeight_ = wantH;
    readbackBuffer_.assign(
        static_cast<size_t>(wantW) * static_cast<size_t>(wantH) * 4u,
        uint8_t{0});
    return true;
}

VirtualTexturePocFrameStats VirtualTexturePoc::tick() {
    VirtualTexturePocFrameStats stats;
    if (!isReady() || !feedbackFbo_) {
        lastStats_ = stats;
        return stats;
    }

    // 1) feedback pass:骨架只 begin/clear/end(page-id 片元 shader 未接,见头注)。
    //    真机上 stall 在回读,与此 pass 画了什么无关,故骨架计时依然有效。
    {
        const double startMs = perf::nowMs();
        if (device_->beginPass(feedbackFbo_.get())) {
            device_->endPass();
        }
        stats.feedbackPassMs = perf::nowMs() - startMs;
    }

    // 2) GPU→CPU 回读(**①的核心测量**):等管线冲刷,移动端可能 stall。
    size_t bytesRead = 0;
    {
        const double startMs = perf::nowMs();
        bytesRead = device_->readFramebufferPixels(
            feedbackFbo_.get(), 0, 0, feedbackWidth_, feedbackHeight_,
            readbackBuffer_.data(), readbackBuffer_.size());
        stats.readbackMs = perf::nowMs() - startMs;
    }
    stats.readbackOk = bytesRead > 0;

    // 3) 解码可见页 → 页表驻留 → 写间接纹理。
    {
        const double startMs = perf::nowMs();
        if (stats.readbackOk) {
            const std::vector<VtPageId> visible = decodeFeedbackPixels(
                readbackBuffer_.data(), feedbackWidth_, feedbackHeight_,
                static_cast<size_t>(feedbackWidth_) * 4u);
            std::vector<VtIndirectionUpdate> updates;
            const VtPageTableStats pt =
                pageTable_->registerVisible(visible, &updates);
            writeIndirection(updates);
            stats.visiblePages = pt.visibleCount;
            stats.residentPages = pt.residentCount;
            stats.newlyResident = pt.newlyResident;
            stats.evicted = pt.evicted;
            stats.thrashed = pt.thrashed;
        }
        stats.updateMs = perf::nowMs() - startMs;
    }

    lastStats_ = stats;
    return stats;
}

void VirtualTexturePoc::writeIndirection(
    const std::vector<VtIndirectionUpdate>& updates) {
    if (updates.empty() || indirectionCpu_.empty()) {
        return;
    }
    const int size = config_.indirectionSize;
    for (const VtIndirectionUpdate& u : updates) {
        // 虚拟页 → 间接纹理 texel:骨架用页坐标对间接纹理尺寸取模映射(占位)。
        // 真实映射(虚拟页网格 → 间接 texel)属 shader 子步,与采样公式配套定。
        const int tx = static_cast<int>(u.page.x % static_cast<uint32_t>(size));
        const int ty = static_cast<int>(u.page.y % static_cast<uint32_t>(size));
        const size_t off =
            (static_cast<size_t>(ty) * static_cast<size_t>(size) +
             static_cast<size_t>(tx)) *
            4u;
        const std::array<uint8_t, 4> texel = encodeSlotTexel(u.slot);
        std::copy(texel.begin(), texel.end(), indirectionCpu_.begin() + off);
    }
    // 骨架:整张回传(简单)。真实实现应按 dirty 矩形局部 updateTextureRegion。
    device_->updateTextureRegion(
        indirectionTexture_.get(), 0, 0, size, size,
        indirectionCpu_.data(),
        static_cast<size_t>(size) * 4u);
}

int64_t VirtualTexturePoc::atlasBytes() const {
    if (!atlasTexture_) {
        return 0;
    }
    return static_cast<int64_t>(atlasTexture_->sizeBytes());
}

void VirtualTexturePoc::dispose() {
    feedbackFbo_.reset();
    indirectionTexture_.reset();
    atlasTexture_.reset();
    pageTable_.reset();
    readbackBuffer_.clear();
    indirectionCpu_.clear();
    feedbackWidth_ = 0;
    feedbackHeight_ = 0;
    lastStats_ = VirtualTexturePocFrameStats{};
}

}  // namespace earth_engine
