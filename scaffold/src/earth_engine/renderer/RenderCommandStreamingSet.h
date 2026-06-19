#pragma once

#include "RenderCommand.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

/// Maintains a stable set of render commands keyed by renderable identity.
///
/// Tiles still produce per-frame candidate commands while selection/refinement
/// evolves, but this set applies those candidates as small mutations to
/// long-lived command slots. The frame submit list then references the active
/// slots in the current streaming order instead of treating tile rendering as a
/// wholly rebuilt immediate-mode list.
class RenderCommandStreamingSet {
public:
    void update(const RenderCommandList& candidates,
                uint64_t frameId,
                RenderCommandList& activeCommands);

    size_t storedCommandCount() const { return commands_.size(); }
    size_t activeCommandCount() const { return activeKeys_.size(); }

private:
    struct Entry {
        RenderCommand command;
        uint64_t lastActiveFrameId = 0;
    };

    std::unordered_map<std::string, Entry> commands_;
    std::vector<std::string> activeKeys_;
};

} // namespace earth_engine
