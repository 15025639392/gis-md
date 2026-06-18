#pragma once

#include "TileLoadTypes.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace earth_engine {

class TileLoadQueue {
public:
    void queue(const TileKey& key,
               TileLoadPriorityGroup group,
               double priority);
    void erase(const TileKey& key);
    template <typename Predicate>
    void eraseIf(Predicate predicate) {
        requests_.erase(
            std::remove_if(requests_.begin(), requests_.end(), predicate),
            requests_.end());
    }
    void clear();
    void resize(size_t size);

    bool empty() const;
    size_t size() const;
    const TileLoadRequest& front() const;

    const std::vector<TileLoadRequest>& requests() const;
    std::vector<TileLoadRequest>::const_iterator begin() const;
    std::vector<TileLoadRequest>::const_iterator end() const;

private:
    std::vector<TileLoadRequest> requests_;
};

} // namespace earth_engine
