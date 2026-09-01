#include "GlyphAtlas.h"

#include "../core/async/WorkLedger.h"
#include "../debug/PlatformLog.h"
#include "RenderDevice.h"

#if __has_include(<stb_truetype.h>)
#include <stb_truetype.h>
#define EARTH_ENGINE_HAS_STB_TRUETYPE 1
#else
#define EARTH_ENGINE_HAS_STB_TRUETYPE 0
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(__ANDROID__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace earth_engine {

struct GlyphAtlas::Impl {
    RenderDevice* device = nullptr;
    std::shared_ptr<std::vector<uint8_t>> fontData;
    bool fontReady = false;
    bool amapProvider = false;
    std::function<void(uint32_t)> providerDemand;
    std::unordered_set<uint32_t> providerDemanded;
#if EARTH_ENGINE_HAS_STB_TRUETYPE
    stbtt_fontinfo font{};
#endif
    float scale = 0.0f;
    float ascentPx = 0.0f;
    float descentPx = 0.0f;

    std::unique_ptr<Texture> texture;
    uint64_t revision = 0;
    // shelf 打包游标
    int cursorX = 0;
    int cursorY = 0;
    int rowHeight = 0;

    std::unordered_map<uint32_t, Glyph> glyphs;
    // 页满丢字计数(见 GlyphAtlas::atlasFullDropCount)。
    int atlasFullDrops = 0;
    bool nearFullWarned = false;

    uint64_t glyphBudgetFrameId = std::numeric_limits<uint64_t>::max();
    double glyphBudgetRemainingMs = 0.0;
    double glyphRasterMs = 0.0;
    size_t glyphRasterAttempts = 0;

#if defined(__ANDROID__) && EARTH_ENGINE_HAS_STB_TRUETYPE
    struct GlyphRasterResult {
        uint64_t generation = 0;
        uint32_t codepoint = 0;
        GlyphAtlas::Glyph glyph;
        std::vector<uint8_t> pixels;
        int width = 0;
        int height = 0;
        bool supported = false;
    };
    struct GlyphRasterLandingBatch {
        GlyphRasterLandingBatch()
            : ticket(WorkLedger::shared().acquire(
                  WorkLedger::Kind::Landing, "glyphRasterBatch")) {}

        void add() {
            std::lock_guard<std::mutex> lock(mutex);
            ++remaining;
        }

        void complete() {
            std::lock_guard<std::mutex> lock(mutex);
            if (remaining > 0) --remaining;
            releaseIfDone();
        }

        void seal() {
            std::lock_guard<std::mutex> lock(mutex);
            sealed = true;
            releaseIfDone();
        }

        void releaseIfDone() {
            if (sealed && remaining == 0) ticket.release();
        }

        std::mutex mutex;
        size_t remaining = 0;
        bool sealed = false;
        WorkLedger::Ticket ticket;
    };

    struct GlyphRasterRequest {
        uint64_t generation = 0;
        uint32_t codepoint = 0;
        std::shared_ptr<std::vector<uint8_t>> fontBytes;
        float scale = 0.0f;
        std::shared_ptr<GlyphRasterLandingBatch> landingBatch;
    };

    // 字形 SDF 不与瓦片解码/地形/内容加载共用 AsyncSystem 全局池。更重要
    // 的是渲染线程提交采用 try_lock + 预留固定小队列：冷启动时即使 worker
    // 正在回收结果也只推迟到下一帧，绝不在 mutex/allocator 上等待 3-8ms。
    // 请求/结果都只在 worker 外做 move，昂贵 SDF 与像素 vector 分配不持锁。
    class GlyphRasterWorkers {
    public:
        static constexpr size_t kBatchSize = 32;

        GlyphRasterWorkers() {
            requests_.reserve(kBatchSize);
            results_.reserve(kBatchSize);
            workers_.reserve(2);
            for (int i = 0; i < 2; ++i) {
                workers_.emplace_back([this]() { run(); });
            }
        }

        ~GlyphRasterWorkers() {
            sealBatch();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stop_ = true;
            }
            cv_.notify_all();
            for (std::thread& worker : workers_) {
                if (worker.joinable()) worker.join();
            }
        }

        bool trySubmit(GlyphRasterRequest request) {
            std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock() || stop_ ||
                requests_.size() >= kBatchSize) {
                return false;
            }
            if (!currentBatch_) {
                currentBatch_ =
                    std::make_shared<GlyphRasterLandingBatch>();
            }
            request.landingBatch = currentBatch_;
            request.landingBatch->add();
            requests_.push_back(std::move(request));
            lock.unlock();
            cv_.notify_one();
            return true;
        }

        void sealBatch() {
            std::shared_ptr<GlyphRasterLandingBatch> batch;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                batch = std::move(currentBatch_);
            }
            if (batch) batch->seal();
        }

        bool tryTake(GlyphRasterResult& result) {
            std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock() || results_.empty()) return false;
            result = std::move(results_.back());
            results_.pop_back();
            readyResults_.fetch_sub(1, std::memory_order_release);
            return true;
        }

        /// 已完成但尚未由渲染线程消费的结果。Landing 票在 worker 把结果
        /// 入箱后释放；一帧最多消费一个 batch，多个 FeatureRenderLayer
        /// 可能在同一帧封口多张 batch，因此剩余结果必须继续持 Pumped
        /// 票，否则第一次 Landing 唤醒消费 32 个后，余下结果没有任何
        /// 事件再唤醒渲染循环。
        bool hasResults() const {
            return readyResults_.load(std::memory_order_acquire) != 0;
        }

    private:
        static GlyphRasterResult rasterize(const GlyphRasterRequest& request) {
            GlyphRasterResult result;
            result.generation = request.generation;
            result.codepoint = request.codepoint;
            if (!request.fontBytes) return result;
            stbtt_fontinfo font{};
            const int offset = stbtt_GetFontOffsetForIndex(
                request.fontBytes->data(), 0);
            if (offset < 0 ||
                !stbtt_InitFont(&font, request.fontBytes->data(), offset)) {
                return result;
            }
            const int glyphIndex = stbtt_FindGlyphIndex(
                &font, static_cast<int>(request.codepoint));
            if (glyphIndex == 0 && request.codepoint != 0x20) return result;
            int advanceUnits = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&font, glyphIndex, &advanceUnits, &lsb);
            result.glyph.advance =
                static_cast<float>(advanceUnits) * request.scale;
            int w = 0, h = 0, xoff = 0, yoff = 0;
            unsigned char* sdf = stbtt_GetGlyphSDF(
                &font, request.scale, glyphIndex, kSdfPadding, kSdfOnEdge,
                kSdfDistScale, &w, &h, &xoff, &yoff);
            result.supported = true;
            result.glyph.offsetX = static_cast<float>(xoff);
            result.glyph.offsetY = static_cast<float>(-yoff);
            if (!sdf || w <= 0 || h <= 0) {
                if (sdf) stbtt_FreeSDF(sdf, nullptr);
                result.glyph.hasBitmap = false;
                return result;
            }
            result.width = w;
            result.height = h;
            result.glyph.width = static_cast<float>(w);
            result.glyph.height = static_cast<float>(h);
            result.glyph.hasBitmap = true;
            result.pixels.assign(sdf, sdf + static_cast<size_t>(w) * h);
            stbtt_FreeSDF(sdf, nullptr);
            return result;
        }

        void run() {
#if defined(__ANDROID__)
            // SDF 是加载期后台工作；绝不能与 GLES/渲染线程争同等调度优先级。
            // nice 10 仅延后字形就绪，不改变标签数量、SDF 尺寸或图集内容。
            const pid_t tid =
                static_cast<pid_t>(syscall(SYS_gettid));
            (void)setpriority(PRIO_PROCESS, tid, 10);
#endif
            while (true) {
                GlyphRasterRequest request;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock,
                             [this]() { return stop_ || !requests_.empty(); });
                    if (stop_ && requests_.empty()) return;
                    request = std::move(requests_.back());
                    requests_.pop_back();
                }
                GlyphRasterResult result = rasterize(request);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    results_.push_back(std::move(result));
                    readyResults_.fetch_add(1, std::memory_order_release);
                }
                // 必须先入箱再完成批次计数；最后一个结果只触发一次唤醒。
                request.landingBatch->complete();
            }
        }

        std::mutex mutex_;
        std::condition_variable cv_;
        bool stop_ = false;
        std::atomic<size_t> readyResults_{0};
        std::vector<GlyphRasterRequest> requests_;
        std::vector<GlyphRasterResult> results_;
        std::vector<std::thread> workers_;
        std::shared_ptr<GlyphRasterLandingBatch> currentBatch_;
    };

    std::unique_ptr<GlyphRasterWorkers> glyphWorkers;
    uint64_t fontGeneration = 0;
    std::unordered_set<uint32_t> pendingGlyphs;
    std::unordered_set<uint32_t> missingGlyphs;
#endif
};

GlyphAtlas::GlyphAtlas(RenderDevice* device)
    : impl_(std::make_unique<Impl>()) {
    impl_->device = device;
}

GlyphAtlas::~GlyphAtlas() = default;

bool GlyphAtlas::setFontData(std::vector<uint8_t> fontData) {
#if EARTH_ENGINE_HAS_STB_TRUETYPE
    if (!impl_->device || fontData.empty() || impl_->amapProvider) return false;
#if defined(__ANDROID__)
    if (!impl_->glyphWorkers) {
        impl_->glyphWorkers = std::make_unique<Impl::GlyphRasterWorkers>();
    }
#endif
    impl_->fontData = std::make_shared<std::vector<uint8_t>>(
        std::move(fontData));
    const int offset =
        stbtt_GetFontOffsetForIndex(impl_->fontData->data(), 0);
    if (offset < 0 ||
        !stbtt_InitFont(&impl_->font, impl_->fontData->data(), offset)) {
        impl_->fontReady = false;
        return false;
    }
    impl_->scale =
        stbtt_ScaleForPixelHeight(&impl_->font, kGlyphPixelHeight);
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&impl_->font, &ascent, &descent, &lineGap);
    impl_->ascentPx = static_cast<float>(ascent) * impl_->scale;
    impl_->descentPx = static_cast<float>(-descent) * impl_->scale;

    // R8 texture array(layer 0):SDF 原生单通道，省 4× 显存；Android
    // array region 复用已验证的 PBO 上传路径，避开普通 2D 热更新产生的
    // 60-90ms 同步。第二层只用于选择 array 资源类型，保留未来多页扩展。
    if (!impl_->texture) {
        TextureDesc desc;
        desc.width = kAtlasSize;
        desc.height = kAtlasSize;
        desc.arrayLayers = 2;
        desc.format = TextureDesc::Format::R8;
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
    impl_->glyphBudgetFrameId = std::numeric_limits<uint64_t>::max();
    impl_->glyphBudgetRemainingMs = 0.0;
    impl_->glyphRasterMs = 0.0;
    impl_->glyphRasterAttempts = 0;
#if defined(__ANDROID__) && EARTH_ENGINE_HAS_STB_TRUETYPE
    ++impl_->fontGeneration;
    impl_->pendingGlyphs.clear();
    impl_->missingGlyphs.clear();
#endif
    impl_->fontReady = impl_->texture != nullptr;
    if (!impl_->fontReady) return false;
    ++impl_->revision;
    return true;
#else
    (void)fontData;
    return false;
#endif
}

void GlyphAtlas::clearFontData() {
    impl_->fontReady = false;
    impl_->fontData.reset();
    impl_->glyphs.clear();
    impl_->cursorX = 0;
    impl_->cursorY = 0;
    impl_->rowHeight = 0;
    impl_->glyphBudgetFrameId = std::numeric_limits<uint64_t>::max();
    impl_->glyphBudgetRemainingMs = 0.0;
    impl_->glyphRasterMs = 0.0;
    impl_->glyphRasterAttempts = 0;
#if defined(__ANDROID__) && EARTH_ENGINE_HAS_STB_TRUETYPE
    ++impl_->fontGeneration;
    impl_->pendingGlyphs.clear();
    impl_->missingGlyphs.clear();
#endif
    ++impl_->revision;
}

bool GlyphAtlas::ready() const { return impl_->fontReady; }
float GlyphAtlas::metricPixelHeight() const {
    return impl_->amapProvider ? static_cast<float>(kAmapProviderPixelHeight)
                               : static_cast<float>(kGlyphPixelHeight);
}
int GlyphAtlas::atlasFullDropCount() const { return impl_->atlasFullDrops; }
float GlyphAtlas::ascent() const { return impl_->ascentPx; }
float GlyphAtlas::descent() const { return impl_->descentPx; }
Texture* GlyphAtlas::texture() { return impl_->texture.get(); }

const Texture* GlyphAtlas::texture() const { return impl_->texture.get(); }

uint64_t GlyphAtlas::revision() const { return impl_->revision; }

size_t GlyphAtlas::residentGlyphCount() const { return impl_->glyphs.size(); }

int GlyphAtlas::shelfUsedHeightPx() const {
    return impl_->cursorY + impl_->rowHeight;
}

bool GlyphAtlas::hasGlyph(uint32_t codepoint) const {
    return impl_->glyphs.find(codepoint) != impl_->glyphs.end();
}

void GlyphAtlas::beginFrameGlyphBudget(uint64_t frameId, double budgetMs) {
    if (impl_->glyphBudgetFrameId == frameId) return;
#if defined(__ANDROID__) && EARTH_ENGINE_HAS_STB_TRUETYPE
    const auto drainStart = std::chrono::steady_clock::now();
#endif
    impl_->glyphBudgetFrameId = frameId;
    impl_->glyphBudgetRemainingMs = std::max(0.0, budgetMs);
    impl_->glyphRasterMs = 0.0;
    impl_->glyphRasterAttempts = 0;
#if defined(__ANDROID__) && EARTH_ENGINE_HAS_STB_TRUETYPE
    // 后台 SDF 任务只在渲染线程回收:纹理上传、shelf 打包和 glyphs 表仍
    // 保持单线程。旧字体 generation 的迟到结果直接丢弃。
    auto commitRaster = [&](Impl::GlyphRasterResult&& result) {
        if (result.generation != impl_->fontGeneration ||
            !impl_->fontReady || result.codepoint == 0) {
            return;
        }
        if (!result.supported) {
            impl_->missingGlyphs.insert(result.codepoint);
            return;
        }
        if (!result.glyph.hasBitmap) {
            impl_->glyphs.emplace(result.codepoint, result.glyph);
            return;
        }
        const int w = result.width;
        const int h = result.height;
        if (w <= 0 || h <= 0 || result.pixels.empty()) return;
        int nextCursorX = impl_->cursorX;
        int nextCursorY = impl_->cursorY;
        int nextRowHeight = impl_->rowHeight;
        if (nextCursorX + w > kAtlasSize) {
            nextCursorX = 0;
            nextCursorY += nextRowHeight;
            nextRowHeight = 0;
        }
        if (nextCursorY + h > kAtlasSize) {
            if (impl_->atlasFullDrops++ == 0) {
                platformLog(LogLevel::Warning, "GlyphAtlas",
                            "atlas page full — further glyphs will not "
                            "render (check atlasFullDropCount)");
            }
            impl_->missingGlyphs.insert(result.codepoint);
            return;
        }
        const int px = nextCursorX;
        const int py = nextCursorY;
        nextCursorX += w;
        if (h > nextRowHeight) nextRowHeight = h;
        if (!impl_->device->updateTextureRegion(
            impl_->texture.get(), px, py, w, h, result.pixels.data(),
            static_cast<size_t>(w), 0)) {
            platformLog(LogLevel::Warning, "GlyphAtlas",
                        "glyph upload failed codepoint=%u; retrying",
                        result.codepoint);
            return;
        }
        impl_->cursorX = nextCursorX;
        impl_->cursorY = nextCursorY;
        impl_->rowHeight = nextRowHeight;
        if (!impl_->nearFullWarned &&
            impl_->cursorY + impl_->rowHeight > kAtlasSize * 4 / 5) {
            impl_->nearFullWarned = true;
            platformLog(LogLevel::Warning, "GlyphAtlas",
                        "shelf %d/%d (80%%) with %zu glyphs — 满后新字形"
                        "永久不渲染",
                        impl_->cursorY + impl_->rowHeight, kAtlasSize,
                        impl_->glyphs.size());
        }
        const float inv = 1.0f / static_cast<float>(kAtlasSize);
        result.glyph.u0 = static_cast<float>(px) * inv;
        result.glyph.v0 = static_cast<float>(py) * inv;
        result.glyph.u1 = static_cast<float>(px + w) * inv;
        result.glyph.v1 = static_cast<float>(py + h) * inv;
        impl_->glyphs.emplace(result.codepoint, result.glyph);
    };
    // 一次消费一个完整后台批次：把“每 2 个字形重画约 900 条命令”收敛成
    // “每批至多 32 个字形重画一次”。32 个 R8 小 region 仍是有界上传，
    // 真机用 drain/build/帧时间验证尖峰，不能靠降低标签数量规避。
    size_t committed = 0;
    Impl::GlyphRasterResult result;
    while (impl_->glyphWorkers &&
           committed < Impl::GlyphRasterWorkers::kBatchSize &&
           impl_->glyphWorkers->tryTake(result)) {
        if (result.generation == impl_->fontGeneration) {
            impl_->pendingGlyphs.erase(result.codepoint);
        }
        commitRaster(std::move(result));
        ++committed;
    }
    const double drainMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - drainStart)
            .count();
    if (drainMs > 4.0) {
        platformLog(LogLevel::Info, "GlyphAtlasPerf",
                    "frame=%llu drain=%.2fms committed=%zu jobs=%zu "
                    "resident=%zu",
                    static_cast<unsigned long long>(frameId), drainMs,
                    committed, impl_->pendingGlyphs.size(),
                    impl_->glyphs.size());
    }
#endif
}

GlyphAtlas::BudgetedGlyphResult GlyphAtlas::ensureGlyphBudgeted(
    uint32_t codepoint) {
    if (impl_->amapProvider) {
        if (hasGlyph(codepoint)) return BudgetedGlyphResult::Ready;
        if (codepoint == 0 || codepoint > 0x10ffff) {
            return BudgetedGlyphResult::MissingTerminal;
        }
        if (impl_->providerDemanded.insert(codepoint).second &&
            impl_->providerDemand) {
            impl_->providerDemand(codepoint);
        }
        return BudgetedGlyphResult::Deferred;
    }
#if defined(__ANDROID__) && EARTH_ENGINE_HAS_STB_TRUETYPE
    if (hasGlyph(codepoint)) return BudgetedGlyphResult::Ready;
    if (impl_->missingGlyphs.count(codepoint) != 0) {
        return BudgetedGlyphResult::MissingTerminal;
    }
    if (!impl_->fontReady || !impl_->fontData) {
        return BudgetedGlyphResult::MissingTerminal;
    }
    // 有界后台并发 + 每帧启动上限:不让一个高德视野把线程池队列灌满,
    // 也不在 UI/渲染线程执行不可抢占的 stbtt_GetGlyphSDF。饱和检查必须
    // 早于 pendingGlyphs:一旦全局已满，本层后续桶本帧同样不可能推进，
    // 消费方据此停止整层扫描；若先返回 Deferred，会把同一批在途字形在
    // 全部 77 个桶中重复查询一遍。
    if (impl_->pendingGlyphs.size() >= Impl::GlyphRasterWorkers::kBatchSize ||
        impl_->glyphRasterAttempts >= Impl::GlyphRasterWorkers::kBatchSize) {
        return BudgetedGlyphResult::Saturated;
    }
    if (impl_->pendingGlyphs.count(codepoint) != 0) {
        return BudgetedGlyphResult::Deferred;
    }
    Impl::GlyphRasterRequest request;
    request.generation = impl_->fontGeneration;
    request.codepoint = codepoint;
    request.fontBytes = impl_->fontData;
    request.scale = impl_->scale;
    if (!impl_->glyphWorkers ||
        !impl_->glyphWorkers->trySubmit(std::move(request))) {
        return BudgetedGlyphResult::Saturated;
    }
    impl_->pendingGlyphs.insert(codepoint);
    ++impl_->glyphRasterAttempts;
    return BudgetedGlyphResult::Deferred;
#else
    if (hasGlyph(codepoint)) return BudgetedGlyphResult::Ready;
    if (impl_->glyphRasterAttempts > 0 &&
        impl_->glyphBudgetRemainingMs <= 0.0) {
        return BudgetedGlyphResult::Saturated;
    }

    const auto start = std::chrono::steady_clock::now();
    const Glyph* glyph = ensureGlyph(codepoint);
    const double elapsedMs = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
    ++impl_->glyphRasterAttempts;
    impl_->glyphRasterMs += elapsedMs;
    impl_->glyphBudgetRemainingMs -= elapsedMs;
    return glyph ? BudgetedGlyphResult::Ready
                 : BudgetedGlyphResult::MissingTerminal;
#endif
}

size_t GlyphAtlas::frameGlyphRasterAttempts() const {
    return impl_->glyphRasterAttempts;
}

void GlyphAtlas::finishGlyphRasterDispatch() {
#if defined(__ANDROID__) && EARTH_ENGINE_HAS_STB_TRUETYPE
    if (impl_->glyphWorkers) impl_->glyphWorkers->sealBatch();
#endif
}

double GlyphAtlas::frameGlyphRasterMs() const {
    return impl_->glyphRasterMs;
}

bool GlyphAtlas::needsFrameForGlyphRasterDispatch() const {
    if (impl_->amapProvider) return false;
#if defined(__ANDROID__) && EARTH_ENGINE_HAS_STB_TRUETYPE
    // Landing 只负责把 worker 从睡眠中唤醒一次；若同一唤醒前积累了
    // 多张 batch，beginFrameGlyphBudget 可能只消费其中一张。结果箱仍有
    // 产物时必须持 Pumped 票继续排空，不能只看 pendingGlyphs(它还包含
    // 已完成但尚未消费的 codepoint)，否则会在首个消费帧后冻在半成品。
    if (impl_->glyphWorkers && impl_->glyphWorkers->hasResults()) {
        return true;
    }
    // pendingGlyphs 只在渲染线程读写；worker 只投递 results 并释放 Landing。
    // 成功提交但 worker 已极快完成时，pending 仍要等下一帧 drain 才会清；
    // attempts 同时覆盖“本帧刚提交、不能因为尚未填满并发槽而空转”的窗口。
    return impl_->pendingGlyphs.empty() && impl_->glyphRasterAttempts == 0;
#else
    return true;
#endif
}

size_t GlyphAtlas::pendingGlyphRasterCount() const {
#if defined(__ANDROID__) && EARTH_ENGINE_HAS_STB_TRUETYPE
    return impl_->pendingGlyphs.size();
#else
    return 0;
#endif
}

const GlyphAtlas::Glyph* GlyphAtlas::ensureGlyph(uint32_t codepoint) {
    if (impl_->amapProvider) {
        auto it = impl_->glyphs.find(codepoint);
        if (it != impl_->glyphs.end()) return &it->second;
        if (impl_->providerDemanded.insert(codepoint).second &&
            impl_->providerDemand) {
            impl_->providerDemand(codepoint);
        }
        return nullptr;
    }
#if EARTH_ENGINE_HAS_STB_TRUETYPE
    if (!impl_->fontReady) return nullptr;
#if defined(__ANDROID__)
    if (impl_->missingGlyphs.count(codepoint) != 0) return nullptr;
#endif
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
    int nextCursorX = impl_->cursorX;
    int nextCursorY = impl_->cursorY;
    int nextRowHeight = impl_->rowHeight;
    if (nextCursorX + w > kAtlasSize) {
        nextCursorX = 0;
        nextCursorY += nextRowHeight;
        nextRowHeight = 0;
    }
    if (nextCursorY + h > kAtlasSize) {
        stbtt_FreeSDF(sdf, nullptr);
        // 图集满(多页/LRU 后置):计数 + 首次告警,不允许静默丢字。
        if (impl_->atlasFullDrops++ == 0) {
            platformLog(LogLevel::Warning, "GlyphAtlas",
                        "atlas page full — further glyphs will not render "
                        "(check atlasFullDropCount)");
        }
        return nullptr;
    }
    const int px = nextCursorX;
    const int py = nextCursorY;
    nextCursorX += w;
    if (h > nextRowHeight) nextRowHeight = h;
    // 逼近告警:满了之后是**永久丢字**(无淘汰,且 appendLabelTextQuads 对
    // 缺字形 `continue` —— 表现是"幸福广场"变"幸广场",比丢整条标签更难
    // 察觉)。只等 atlasFullDropCount 报第一次已经晚了,故在 80% 先喊一声。
    std::vector<uint8_t> pixels(
        sdf, sdf + static_cast<size_t>(w) * h);
    stbtt_FreeSDF(sdf, nullptr);
    if (!impl_->device->updateTextureRegion(
        impl_->texture.get(), px, py, w, h, pixels.data(),
        static_cast<size_t>(w), 0)) {
        platformLog(LogLevel::Warning, "GlyphAtlas",
                    "glyph upload failed codepoint=%u; retrying", codepoint);
        return nullptr;
    }

    impl_->cursorX = nextCursorX;
    impl_->cursorY = nextCursorY;
    impl_->rowHeight = nextRowHeight;
    if (!impl_->nearFullWarned &&
        impl_->cursorY + impl_->rowHeight > kAtlasSize * 4 / 5) {
        impl_->nearFullWarned = true;
        platformLog(LogLevel::Warning, "GlyphAtlas",
                    "shelf %d/%d (80%%) with %zu glyphs — 满后新字形永久不"
                    "渲染(缺字非缺标签)。该上多页/LRU 了",
                    impl_->cursorY + impl_->rowHeight, kAtlasSize,
                    impl_->glyphs.size());
    }

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

void GlyphAtlas::activateAmapOfficialProvider(
    std::function<void(uint32_t)> demand) {
    clearFontData();
    impl_->amapProvider = true;
    impl_->providerDemand = std::move(demand);
    impl_->fontReady = impl_->device != nullptr;
    impl_->ascentPx = static_cast<float>(kAmapProviderPixelHeight);
    impl_->descentPx = 0.0f;
    if (!impl_->texture && impl_->device) {
        TextureDesc desc;
        desc.width = kAtlasSize;
        desc.height = kAtlasSize;
        desc.arrayLayers = 2;
        desc.format = TextureDesc::Format::R8;
        desc.mipmap = false;
        desc.minFilter = TextureDesc::Filter::Linear;
        desc.magFilter = TextureDesc::Filter::Linear;
        impl_->texture = impl_->device->createTexture(desc);
        impl_->fontReady = impl_->texture != nullptr;
    }
    ++impl_->revision;
}

bool GlyphAtlas::installAmapOfficialGlyphBatch(
    int imageWidth, int imageHeight,
    const std::vector<uint8_t>& grayscale,
    const std::vector<ProviderGlyph>& glyphs) {
    if (!impl_->amapProvider || !impl_->fontReady || !impl_->texture ||
        imageWidth <= 0 || imageHeight <= 0 ||
        grayscale.size() != static_cast<size_t>(imageWidth) * imageHeight ||
        glyphs.empty()) return false;
    for (const ProviderGlyph& source : glyphs) {
        if (source.codepoint == 0 || source.fontWidth <= 0 ||
            source.fontHeight <= 0 || source.horiAdvance < 0 ||
            source.posX < 0 || source.posY < 0 ||
            source.posX + source.fontWidth > imageWidth ||
            source.posY + source.fontHeight > imageHeight ||
            impl_->glyphs.count(source.codepoint) != 0) return false;
    }
    struct Pending { ProviderGlyph source; Glyph glyph; int x; int y; };
    std::vector<Pending> pending;
    int cursorX = impl_->cursorX, cursorY = impl_->cursorY;
    int rowHeight = impl_->rowHeight;
    for (const ProviderGlyph& source : glyphs) {
        if (cursorX + source.fontWidth > kAtlasSize) {
            cursorX = 0;
            cursorY += rowHeight;
            rowHeight = 0;
        }
        if (cursorY + source.fontHeight > kAtlasSize) return false;
        Glyph glyph;
        glyph.offsetX = static_cast<float>(source.horiBearingX);
        glyph.offsetY = static_cast<float>(-source.horiBearingY);
        glyph.width = static_cast<float>(source.fontWidth);
        glyph.height = static_cast<float>(source.fontHeight);
        glyph.advance = static_cast<float>(source.horiAdvance + 1);
        glyph.hasBitmap = true;
        pending.push_back({source, glyph, cursorX, cursorY});
        cursorX += source.fontWidth;
        rowHeight = std::max(rowHeight, source.fontHeight);
    }
    for (Pending& item : pending) {
        std::vector<uint8_t> pixels(
            static_cast<size_t>(item.source.fontWidth) *
            item.source.fontHeight);
        for (int y = 0; y < item.source.fontHeight; ++y) {
            const size_t src = static_cast<size_t>(item.source.posY + y) *
                imageWidth + item.source.posX;
            const size_t dst = static_cast<size_t>(y) * item.source.fontWidth;
            std::copy_n(grayscale.data() + src, item.source.fontWidth,
                        pixels.data() + dst);
        }
        if (!impl_->device->updateTextureRegion(
                impl_->texture.get(), item.x, item.y,
                item.source.fontWidth, item.source.fontHeight, pixels.data(),
                static_cast<size_t>(item.source.fontWidth), 0)) return false;
        const float inv = 1.0f / static_cast<float>(kAtlasSize);
        item.glyph.u0 = item.x * inv;
        item.glyph.v0 = item.y * inv;
        item.glyph.u1 = (item.x + item.source.fontWidth) * inv;
        item.glyph.v1 = (item.y + item.source.fontHeight) * inv;
    }
    impl_->cursorX = cursorX;
    impl_->cursorY = cursorY;
    impl_->rowHeight = rowHeight;
    for (Pending& item : pending) {
        impl_->glyphs.emplace(item.source.codepoint, item.glyph);
    }
    ++impl_->revision;
    return true;
}

void GlyphAtlas::clearAmapOfficialProvider() {
    impl_->amapProvider = false;
    impl_->providerDemand = {};
    impl_->providerDemanded.clear();
    clearFontData();
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
