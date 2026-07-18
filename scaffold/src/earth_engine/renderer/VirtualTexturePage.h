#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>
#include <vector>

namespace earth_engine {

// ============================================================
// 北极星 Phase 2b — 虚拟纹理 PoC 的「页(page)」数据模型
// ============================================================
//
// 纯 CPU、无 GPU 依赖、可完全单测。这是 §7 要求的「从一开始抽象成页」的
// 载体:B(逐瓦片合成)与 C(共享 atlas 虚拟纹理)共用同一套页语义,撞墙时
// 换实现不换数据模型。PoC 的目的是**量移动端固定开销**(feedback 回读 stall
// + 逐 fragment 间接采样),这套模型只负责「哪些页可见 → 哪些页驻留 atlas →
// 间接纹理怎么写」的 CPU 侧账,GPU 侧编排在 VirtualTexturePoc。

/// 一个虚拟纹理页 = 影像四叉树里的一块瓦片,由 (lod, x, y) 唯一标识。
/// x/y 是该 lod 下的页网格坐标(0 .. 2^lod-1)。
struct VtPageId {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t lod = 0;

    bool operator==(const VtPageId& o) const {
        return x == o.x && y == o.y && lod == o.lod;
    }
    bool operator!=(const VtPageId& o) const { return !(*this == o); }
};

struct VtPageIdHash {
    size_t operator()(const VtPageId& p) const {
        // 三坐标混合(splitmix 风格),避免相邻页哈希碰撞成簇。
        uint64_t h = (static_cast<uint64_t>(p.lod) << 56) ^
                     (static_cast<uint64_t>(p.x) << 28) ^
                     static_cast<uint64_t>(p.y);
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        return static_cast<size_t>(h);
    }
};

// ---- feedback 颜色编解码 ----
//
// feedback pass 把每个 fragment「想要的页」写成颜色,读回 CPU 后解码成 VtPageId。
// framebuffer color 格式被两后端硬编码为 RGBA8(见 RenderDevice createFramebuffer),
// 故只有 32 bit 可用。**PoC 位预算**:lod 高 4 位(0-15)+ x 中 14 位 + y 低 14 位。
//   ⚠️ 局限:x/y 各 14 位 → 至多 z14(16384 页/轴)。解耦影像要到 z18 需更宽
//   feedback 格式(RG16/R32UI),要扩 TextureDesc/FramebufferDesc 的 format(现无),
//   属 PoC 之后的跟进项。skeleton 先用 RGBA8 直编码证机制,不阻塞固定开销测量。
namespace vt_codec {

constexpr uint32_t kMaxLod = 15;          // 4 bit
constexpr uint32_t kMaxCoord = (1u << 14) - 1;  // 14 bit → 16383
constexpr uint32_t kBackgroundKey = 0;    // 全 0 = 背景/无请求(lod0 页 (0,0) 无歧义地当背景略过)

/// (lod,x,y) 打进 32 位无符号键。越界坐标被 clamp(PoC 容错,不崩)。
inline uint32_t packKey(const VtPageId& p) {
    const uint32_t lod = p.lod > kMaxLod ? kMaxLod : p.lod;
    const uint32_t x = p.x > kMaxCoord ? kMaxCoord : p.x;
    const uint32_t y = p.y > kMaxCoord ? kMaxCoord : p.y;
    return (lod << 28) | (x << 14) | y;
}

inline VtPageId unpackKey(uint32_t key) {
    VtPageId p;
    p.lod = (key >> 28) & 0xF;
    p.x = (key >> 14) & 0x3FFF;
    p.y = key & 0x3FFF;
    return p;
}

/// 页 → RGBA8 像素(小端:R=低 8 位 … A=高 8 位)。
inline std::array<uint8_t, 4> encode(const VtPageId& p) {
    const uint32_t key = packKey(p);
    return {static_cast<uint8_t>(key & 0xFF),
            static_cast<uint8_t>((key >> 8) & 0xFF),
            static_cast<uint8_t>((key >> 16) & 0xFF),
            static_cast<uint8_t>((key >> 24) & 0xFF)};
}

/// RGBA8 像素 → 32 位键(背景像素 key==0 由调用方判 kBackgroundKey 略过)。
inline uint32_t decodeKey(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(a) << 24);
}

}  // namespace vt_codec

/// 从一整帧 feedback 像素(RGBA8,rowBytes 对齐)里抽出去重后的可见页集合。
/// 背景像素(key==kBackgroundKey)略过。返回按首次出现顺序稳定排列(可复现)。
std::vector<VtPageId> decodeFeedbackPixels(const uint8_t* pixels,
                                           int width,
                                           int height,
                                           size_t rowBytes);

// ---- 页表(atlas 驻留管理) ----
//
// 固定容量的物理 atlas 槽位(pageCapacity)。每帧喂入可见页 → 决定新驻留/LRU
// 淘汰,产出「间接纹理更新」(哪个虚拟页映射到 atlas 的哪个物理槽)。这是 C 的
// 核心 CPU 逻辑;B 复用同一驻留账(只是「槽」变成逐瓦片纹理)。

/// atlas 里一个物理槽的坐标(行列)+ 它当前承载的虚拟页。
struct VtAtlasSlot {
    int column = 0;
    int row = 0;
};

/// 一次页表更新的产物:某虚拟页现在落在 atlas 的某物理槽(供间接纹理写入)。
struct VtIndirectionUpdate {
    VtPageId page;
    VtAtlasSlot slot;
};

/// 一帧 registerVisible 的统计(供固定开销测量与验收)。
struct VtPageTableStats {
    int visibleCount = 0;   // 去重后的可见页数
    int residentCount = 0;  // 帧末 atlas 驻留页数
    int newlyResident = 0;  // 本帧新调入
    int evicted = 0;        // 本帧淘汰
    bool thrashed = false;  // 可见页数 > 容量(atlas 装不下,证明需更大 atlas / 更细定尺)
};

class VtPageTable {
public:
    /// atlasColumns × atlasRows 个物理槽,容量 = 两者乘积。
    VtPageTable(int atlasColumns, int atlasRows);

    /// 喂入本帧可见页(已去重),更新驻留集:新页占空槽或 LRU 淘汰最久未见的页。
    /// outUpdates(可空)收集本帧发生变化的 (page→slot) 映射,供上层写间接纹理。
    /// 返回本帧统计。O(visible + evicted)。
    VtPageTableStats registerVisible(
        const std::vector<VtPageId>& visiblePages,
        std::vector<VtIndirectionUpdate>* outUpdates = nullptr);

    int capacity() const { return capacity_; }
    int residentCount() const { return static_cast<int>(residents_.size()); }
    bool isResident(const VtPageId& p) const {
        return residents_.find(p) != residents_.end();
    }
    /// 查询某页当前物理槽;未驻留返回 false。
    bool slotOf(const VtPageId& p, VtAtlasSlot& outSlot) const;

    /// 清空驻留(surface 重建/context lost 时)。
    void reset();

private:
    VtAtlasSlot slotForIndex(int index) const;

    int atlasColumns_ = 1;
    int atlasRows_ = 1;
    int capacity_ = 1;

    // LRU:list 头=最近使用,尾=最久未用。map 存 page→(list 迭代器, 物理槽 index)。
    struct Resident {
        int slotIndex = 0;
        std::list<VtPageId>::iterator lruIt;
    };
    std::list<VtPageId> lru_;
    std::unordered_map<VtPageId, Resident, VtPageIdHash> residents_;
    std::vector<int> freeSlots_;  // 尚未占用的物理槽 index 栈
};

}  // namespace earth_engine
