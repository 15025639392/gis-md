#include "OverlayStyle.h"

namespace earth_engine {

const InteractionStyleOverride* InteractionStyle::find(const std::string& stateName) const {
    for (const auto& s : states) {
        if (s.state == stateName) return &s;
    }
    return nullptr;
}

} // namespace earth_engine
