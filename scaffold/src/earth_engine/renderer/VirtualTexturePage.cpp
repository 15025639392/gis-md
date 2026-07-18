#include "VirtualTexturePage.h"

#include <unordered_set>

namespace earth_engine {

std::vector<VtPageId> decodeFeedbackPixels(const uint8_t* pixels,
                                           int width,
                                           int height,
                                           size_t rowBytes) {
    std::vector<VtPageId> out;
    if (!pixels || width <= 0 || height <= 0) {
        return out;
    }
    // 去重:同一页在整帧多次出现只收一次;seen 记录已收键,order 保持首次出现序。
    std::unordered_map<uint32_t, bool> seen;
    seen.reserve(256);
    for (int row = 0; row < height; ++row) {
        const uint8_t* line = pixels + static_cast<size_t>(row) * rowBytes;
        for (int col = 0; col < width; ++col) {
            const uint8_t* px = line + static_cast<size_t>(col) * 4u;
            const uint32_t key =
                vt_codec::decodeKey(px[0], px[1], px[2], px[3]);
            if (key == vt_codec::kBackgroundKey) {
                continue;
            }
            if (seen.emplace(key, true).second) {
                out.push_back(vt_codec::unpackKey(key));
            }
        }
    }
    return out;
}

VtPageTable::VtPageTable(int atlasColumns, int atlasRows)
    : atlasColumns_(atlasColumns < 1 ? 1 : atlasColumns),
      atlasRows_(atlasRows < 1 ? 1 : atlasRows) {
    capacity_ = atlasColumns_ * atlasRows_;
    freeSlots_.reserve(capacity_);
    // 逆序压栈 → 先弹出 index 0(确定性,便于测试断言槽分配顺序)。
    for (int i = capacity_ - 1; i >= 0; --i) {
        freeSlots_.push_back(i);
    }
    residents_.reserve(capacity_ * 2);
}

VtAtlasSlot VtPageTable::slotForIndex(int index) const {
    VtAtlasSlot s;
    s.column = index % atlasColumns_;
    s.row = index / atlasColumns_;
    return s;
}

bool VtPageTable::slotOf(const VtPageId& p, VtAtlasSlot& outSlot) const {
    auto it = residents_.find(p);
    if (it == residents_.end()) {
        return false;
    }
    outSlot = slotForIndex(it->second.slotIndex);
    return true;
}

VtPageTableStats VtPageTable::registerVisible(
    const std::vector<VtPageId>& visiblePages,
    std::vector<VtIndirectionUpdate>* outUpdates) {
    VtPageTableStats stats;
    stats.visibleCount = static_cast<int>(visiblePages.size());
    stats.thrashed = stats.visibleCount > capacity_;

    // 1) 已驻留的可见页:提到 LRU 头(touch)。收集本帧可见集合供淘汰保护。
    std::unordered_set<uint32_t> visibleKeys;
    visibleKeys.reserve(visiblePages.size() * 2);
    for (const VtPageId& p : visiblePages) {
        visibleKeys.insert(vt_codec::packKey(p));
        auto it = residents_.find(p);
        if (it != residents_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second.lruIt);
        }
    }

    // 2) 未驻留的可见页:占空槽,或淘汰「最久未用且本帧不可见」的驻留页。
    for (const VtPageId& p : visiblePages) {
        if (residents_.find(p) != residents_.end()) {
            continue;
        }
        int slotIndex = -1;
        if (!freeSlots_.empty()) {
            slotIndex = freeSlots_.back();
            freeSlots_.pop_back();
        } else {
            // 从 LRU 尾向前找第一个本帧不可见的驻留页淘汰。全部可见(thrash)→放弃本页。
            bool evictedOne = false;
            for (auto rit = lru_.rbegin(); rit != lru_.rend(); ++rit) {
                if (visibleKeys.find(vt_codec::packKey(*rit)) !=
                    visibleKeys.end()) {
                    continue;  // 本帧可见,保护不淘汰
                }
                const VtPageId victim = *rit;
                auto victimIt = residents_.find(victim);
                slotIndex = victimIt->second.slotIndex;
                lru_.erase(victimIt->second.lruIt);
                residents_.erase(victimIt);
                ++stats.evicted;
                evictedOne = true;
                break;
            }
            if (!evictedOne) {
                continue;  // atlas 满且全可见,本页这帧调不进(thrash 已记录)
            }
        }
        lru_.push_front(p);
        Resident r;
        r.slotIndex = slotIndex;
        r.lruIt = lru_.begin();
        residents_.emplace(p, r);
        ++stats.newlyResident;
        if (outUpdates) {
            outUpdates->push_back({p, slotForIndex(slotIndex)});
        }
    }

    stats.residentCount = static_cast<int>(residents_.size());
    return stats;
}

void VtPageTable::reset() {
    lru_.clear();
    residents_.clear();
    freeSlots_.clear();
    for (int i = capacity_ - 1; i >= 0; --i) {
        freeSlots_.push_back(i);
    }
}

}  // namespace earth_engine
