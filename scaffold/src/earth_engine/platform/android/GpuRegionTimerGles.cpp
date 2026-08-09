#include "GpuRegionTimerGles.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>

#include <cstring>

namespace earth_engine {
namespace {

// EXT_disjoint_timer_query 的常量/入口点。GLES3 核心里有 glBeginQuery,但
// **没有** TIME_ELAPSED 目标,也没有 64 位结果读取 —— 必须走 EXT 入口,不能
// 图省事用核心那套(会在 glBeginQuery 处报 INVALID_ENUM 然后一路静默为 0)。
#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif
#ifndef GL_QUERY_RESULT_EXT
#define GL_QUERY_RESULT_EXT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE_EXT
#define GL_QUERY_RESULT_AVAILABLE_EXT 0x8867
#endif

using PFNGenQueriesEXT = void (*)(GLsizei, GLuint*);
using PFNDeleteQueriesEXT = void (*)(GLsizei, const GLuint*);
using PFNBeginQueryEXT = void (*)(GLenum, GLuint);
using PFNEndQueryEXT = void (*)(GLenum);
using PFNGetQueryObjectuivEXT = void (*)(GLuint, GLenum, GLuint*);
using PFNGetQueryObjectui64vEXT = void (*)(GLuint, GLenum, GLuint64*);

PFNGenQueriesEXT pGenQueries = nullptr;
PFNDeleteQueriesEXT pDeleteQueries = nullptr;
PFNBeginQueryEXT pBeginQuery = nullptr;
PFNEndQueryEXT pEndQuery = nullptr;
PFNGetQueryObjectuivEXT pGetQueryObjectuiv = nullptr;
PFNGetQueryObjectui64vEXT pGetQueryObjectui64v = nullptr;

bool loadEntryPoints() {
    pGenQueries = reinterpret_cast<PFNGenQueriesEXT>(
        eglGetProcAddress("glGenQueriesEXT"));
    pDeleteQueries = reinterpret_cast<PFNDeleteQueriesEXT>(
        eglGetProcAddress("glDeleteQueriesEXT"));
    pBeginQuery = reinterpret_cast<PFNBeginQueryEXT>(
        eglGetProcAddress("glBeginQueryEXT"));
    pEndQuery = reinterpret_cast<PFNEndQueryEXT>(
        eglGetProcAddress("glEndQueryEXT"));
    pGetQueryObjectuiv = reinterpret_cast<PFNGetQueryObjectuivEXT>(
        eglGetProcAddress("glGetQueryObjectuivEXT"));
    pGetQueryObjectui64v = reinterpret_cast<PFNGetQueryObjectui64vEXT>(
        eglGetProcAddress("glGetQueryObjectui64vEXT"));
    return pGenQueries && pDeleteQueries && pBeginQuery && pEndQuery &&
           pGetQueryObjectuiv && pGetQueryObjectui64v;
}

bool hasExtension(const char* name) {
    const char* exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!exts) return false;
    const size_t len = std::strlen(name);
    for (const char* p = std::strstr(exts, name); p; p = std::strstr(p + 1, name)) {
        const bool leftOk = (p == exts) || (p[-1] == ' ');
        const bool rightOk = (p[len] == ' ' || p[len] == '\0');
        if (leftOk && rightOk) return true;
    }
    return false;
}

}  // namespace

bool GpuRegionTimerGles::initialize() {
    if (available_) return true;
    if (!hasExtension("GL_EXT_disjoint_timer_query")) {
        __android_log_print(ANDROID_LOG_INFO, "GpuPass",
                            "GL_EXT_disjoint_timer_query absent — GPU 区间计时不可用");
        return false;
    }
    if (!loadEntryPoints()) {
        __android_log_print(ANDROID_LOG_WARN, "GpuPass",
                            "扩展在扩展串里但入口点取不全 — 视作不可用");
        return false;
    }
    // 扩展串有、入口点齐,仍可能建不出查询对象(驱动阉割)。当场建一个探一下,
    // 免得后面每帧静默产出 0 段——"打开了但没数"和"没打开"必须分得开。
    GLuint probe = 0;
    pGenQueries(1, &probe);
    if (probe == 0) {
        __android_log_print(ANDROID_LOG_WARN, "GpuPass",
                            "glGenQueriesEXT 产出 0 — 视作不可用");
        return false;
    }
    freeQueries_.push_back(probe);
    available_ = true;
    __android_log_print(ANDROID_LOG_INFO, "GpuPass",
                        "GPU 区间计时已启用 (ring=%d maxRegions=%d)",
                        kFrameRing, kMaxRegionsPerFrame);
    return true;
}

void GpuRegionTimerGles::shutdown() {
    if (!available_) return;
    if (activeQuery_ != 0) {
        pEndQuery(GL_TIME_ELAPSED_EXT);
        activeQuery_ = 0;
    }
    for (FrameSlot& slot : slots_) {
        for (Sample& s : slot.samples) {
            if (s.query) pDeleteQueries(1, &s.query);
        }
        slot.samples.clear();
        slot.pending = false;
    }
    for (unsigned int q : freeQueries_) {
        pDeleteQueries(1, &q);
    }
    freeQueries_.clear();
    available_ = false;
    lastValid_ = false;
}

unsigned int GpuRegionTimerGles::acquireQuery() {
    if (!freeQueries_.empty()) {
        const unsigned int q = freeQueries_.back();
        freeQueries_.pop_back();
        return q;
    }
    GLuint q = 0;
    pGenQueries(1, &q);
    return q;
}

void GpuRegionTimerGles::recycle(FrameSlot& slot) {
    for (Sample& s : slot.samples) {
        if (s.query) freeQueries_.push_back(s.query);
    }
    slot.samples.clear();
    slot.dropped = 0;
    slot.disjoint = false;
    slot.pending = false;
}

void GpuRegionTimerGles::beginFrame(uint64_t frameId) {
    if (!available_) return;
    currentSlot_ = static_cast<int>(frameId % kFrameRing);
    // 环转了一圈这一槽还 pending = GPU 落后超过 kFrameRing 帧(或被 disjoint
    // 作废)。直接丢弃复用——绝不为了拿数去阻塞等 GPU。
    FrameSlot& slot = slots_[currentSlot_];
    recycle(slot);
    slot.frameId = frameId;
    regionsThisFrame_ = 0;
}

void GpuRegionTimerGles::beginRegion(const std::string& name) {
    if (!available_) return;
    endRegion();
    ++regionsThisFrame_;
    FrameSlot& slot = slots_[currentSlot_];
    if (regionsThisFrame_ > kMaxRegionsPerFrame) {
        ++slot.dropped;
        return;
    }
    const unsigned int q = acquireQuery();
    if (q == 0) {
        ++slot.dropped;
        return;
    }
    pBeginQuery(GL_TIME_ELAPSED_EXT, q);
    activeQuery_ = q;
    slot.samples.push_back(Sample{name, q});
}

void GpuRegionTimerGles::endRegion() {
    if (!available_ || activeQuery_ == 0) return;
    pEndQuery(GL_TIME_ELAPSED_EXT);
    activeQuery_ = 0;
}

void GpuRegionTimerGles::endFrame() {
    if (!available_) return;
    endRegion();

    // disjoint 读一次即复位。它覆盖的是"上次读到这次读"之间的窗口,也就是本帧
    // —— 但本帧的查询结果要过几帧才回读得到,所以标记必须挂在槽上带走。
    GLint disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    FrameSlot& current = slots_[currentSlot_];
    if (disjoint) {
        // 保守:窗口内所有还没回读的帧一起作废。分不清是哪一帧被抢占时,宁可
        // 少报几帧,也不能报一个被调频污染过的数当真值。
        for (FrameSlot& slot : slots_) {
            if (slot.pending || &slot == &current) slot.disjoint = true;
        }
    }
    current.pending = !current.samples.empty() || current.dropped > 0;

    // 非阻塞回读:从最老的槽开始,只取已就绪的。
    for (int i = 1; i <= kFrameRing; ++i) {
        FrameSlot& slot = slots_[(currentSlot_ + i) % kFrameRing];
        if (!slot.pending || slot.frameId == current.frameId) continue;
        if (tryResolve(slot)) {
            recycle(slot);
        }
        break;  // 一帧只回读一个槽,均摊 CPU
    }
}

bool GpuRegionTimerGles::tryResolve(FrameSlot& slot) {
    // 全部就绪才算这一帧完成——半帧的和会比真值小,而"偏小的和"正是最容易
    // 被当成"这个 pass 不贵"的读数。
    for (const Sample& s : slot.samples) {
        GLuint ready = 0;
        pGetQueryObjectuiv(s.query, GL_QUERY_RESULT_AVAILABLE_EXT, &ready);
        if (!ready) return false;
    }
    if (slot.disjoint) {
        last_.frameId = slot.frameId;
        last_.regions.clear();
        last_.totalMs = 0.0;
        last_.droppedRegions = slot.dropped;
        last_.disjoint = true;
        lastValid_ = true;
        return true;
    }

    last_.frameId = slot.frameId;
    last_.regions.clear();
    last_.totalMs = 0.0;
    last_.disjoint = false;
    last_.droppedRegions = slot.dropped;
    for (const Sample& s : slot.samples) {
        GLuint64 ns = 0;
        pGetQueryObjectui64v(s.query, GL_QUERY_RESULT_EXT, &ns);
        const double ms = static_cast<double>(ns) / 1.0e6;
        last_.totalMs += ms;
        bool merged = false;
        for (GpuFrameTiming::Region& r : last_.regions) {
            if (r.name == s.name) {
                r.ms += ms;
                ++r.count;
                merged = true;
                break;
            }
        }
        if (!merged) {
            last_.regions.push_back(GpuFrameTiming::Region{s.name, ms, 1});
        }
    }
    lastValid_ = true;
    return true;
}

}  // namespace earth_engine
