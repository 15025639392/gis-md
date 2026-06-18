#pragma once

#include "../core/resources/FrameResourceBudget.h"

#include <algorithm>

namespace earth_engine {

enum class TileLoadPriorityGroup {
    Preload = 0,
    Normal = 1,
    Urgent = 2
};

class TileLoadPriorityPolicy {
public:
    static bool hasHigherPriority(TileLoadPriorityGroup lhsGroup,
                                  double lhsPriority,
                                  TileLoadPriorityGroup rhsGroup,
                                  double rhsPriority);

    static FrameResourcePriority toFramePriority(TileLoadPriorityGroup group);

    template <typename Iterator>
    static Iterator selectHighestPriority(Iterator begin, Iterator end) {
        if (begin == end) {
            return end;
        }
        Iterator best = begin;
        for (Iterator it = std::next(begin); it != end; ++it) {
            if (hasHigherPriority(
                    it->group,
                    it->priority,
                    best->group,
                    best->priority)) {
                best = it;
            }
        }
        return best;
    }

    template <typename Requests>
    static void sortByPriority(Requests& requests) {
        std::sort(requests.begin(), requests.end(),
            [](const auto& lhs, const auto& rhs) {
                return hasHigherPriority(
                    lhs.group,
                    lhs.priority,
                    rhs.group,
                    rhs.priority);
            });
    }
};

} // namespace earth_engine
