#include "TileLoadPriorityPolicy.h"

namespace earth_engine {

bool TileLoadPriorityPolicy::hasHigherPriority(
    TileLoadPriorityGroup lhsGroup,
    double lhsPriority,
    TileLoadPriorityGroup rhsGroup,
    double rhsPriority) {
    if (lhsGroup != rhsGroup) {
        return static_cast<int>(lhsGroup) > static_cast<int>(rhsGroup);
    }
    return lhsPriority < rhsPriority;
}

FrameResourcePriority TileLoadPriorityPolicy::toFramePriority(
    TileLoadPriorityGroup group) {
    switch (group) {
        case TileLoadPriorityGroup::Preload:
            return FrameResourcePriority::Preload;
        case TileLoadPriorityGroup::Normal:
            return FrameResourcePriority::Normal;
        case TileLoadPriorityGroup::Urgent:
            return FrameResourcePriority::Urgent;
    }
    return FrameResourcePriority::Normal;
}

} // namespace earth_engine
