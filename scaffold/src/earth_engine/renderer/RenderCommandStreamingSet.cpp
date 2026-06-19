#include "RenderCommandStreamingSet.h"

#include <unordered_set>

namespace earth_engine {

void RenderCommandStreamingSet::update(const RenderCommandList& candidates,
                                       uint64_t frameId,
                                       RenderCommandList& activeCommands) {
    activeKeys_.clear();
    activeKeys_.reserve(candidates.size());
    std::unordered_set<std::string> activeKeySet;
    activeKeySet.reserve(candidates.size());

    for (const RenderCommand& candidate : candidates) {
        if (candidate.stableKey.empty()) {
            activeCommands.push_back(candidate);
            continue;
        }

        Entry& entry = commands_[candidate.stableKey];
        entry.command = candidate;
        entry.lastActiveFrameId = frameId;
        if (activeKeySet.insert(candidate.stableKey).second) {
            activeKeys_.push_back(candidate.stableKey);
        }
    }

    for (auto it = commands_.begin(); it != commands_.end();) {
        if (it->second.lastActiveFrameId != frameId) {
            it = commands_.erase(it);
        } else {
            ++it;
        }
    }

    for (const std::string& key : activeKeys_) {
        auto it = commands_.find(key);
        if (it != commands_.end()) {
            activeCommands.push_back(it->second.command);
        }
    }
}

} // namespace earth_engine
